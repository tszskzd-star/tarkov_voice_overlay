package main

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"log"
	"net/http"
	"os"
	"sort"
	"sync"
	"time"

	"nhooyr.io/websocket"
)

const maxPeersPerRoom = 5
const maxVoicePayloadBytes = 4096

type Envelope struct {
	Type         string          `json:"type"`
	RoomID       string          `json:"roomId,omitempty"`
	PeerID       string          `json:"peerId,omitempty"`
	TargetPeerID string          `json:"targetPeerId,omitempty"`
	Nick         string          `json:"nick,omitempty"`
	Room         *RoomSnapshot   `json:"room,omitempty"`
	Rooms        []RoomSnapshot  `json:"rooms,omitempty"`
	Payload      json.RawMessage `json:"payload,omitempty"`
	Error        string          `json:"error,omitempty"`
	SentAt       int64           `json:"sentAt,omitempty"`
}

type RoomSnapshot struct {
	ID              string   `json:"id"`
	Name            string   `json:"name"`
	HostPeerID      string   `json:"hostPeerId"`
	HostNick        string   `json:"hostNick"`
	PeerCount       int      `json:"peerCount"`
	MaxPeers        int      `json:"maxPeers"`
	Locked          bool     `json:"locked"`
	LANOnly         bool     `json:"lanOnly"`
	CreatedAtUnixMs int64    `json:"createdAtUnixMs"`
	Peers           []string `json:"peers,omitempty"`
}

type Room struct {
	ID        string
	Name      string
	HostID    string
	HostNick  string
	Locked    bool
	CreatedAt time.Time
	Peers     map[string]*Client
}

func (r *Room) snapshot(includePeers bool) RoomSnapshot {
	peers := []string(nil)
	if includePeers {
		peers = make([]string, 0, len(r.Peers))
		for peerID := range r.Peers {
			peers = append(peers, peerID)
		}
	}

	return RoomSnapshot{
		ID:              r.ID,
		Name:            r.Name,
		HostPeerID:      r.HostID,
		HostNick:        r.HostNick,
		PeerCount:       len(r.Peers),
		MaxPeers:        maxPeersPerRoom,
		Locked:          r.Locked,
		LANOnly:         false,
		CreatedAtUnixMs: r.CreatedAt.UnixMilli(),
		Peers:           peers,
	}
}

func promoteRoomHost(room *Room) {
	if room == nil || len(room.Peers) == 0 {
		return
	}

	ids := make([]string, 0, len(room.Peers))
	for peerID := range room.Peers {
		ids = append(ids, peerID)
	}
	sort.Strings(ids)

	next := room.Peers[ids[0]]
	room.HostID = next.id
	room.HostNick = next.nick
}

type Client struct {
	id     string
	nick   string
	roomID string
	conn   *websocket.Conn
	send   chan Envelope
}

type Hub struct {
	mu      sync.RWMutex
	rooms   map[string]*Room
	clients map[string]*Client
}

func NewHub() *Hub {
	return &Hub{
		rooms:   make(map[string]*Room),
		clients: make(map[string]*Client),
	}
}

func (h *Hub) addClient(c *Client) {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.clients[c.id] = c
}

func (h *Hub) removeClient(c *Client) {
	h.mu.Lock()
	defer h.mu.Unlock()

	delete(h.clients, c.id)
	if c.roomID == "" {
		return
	}

	room, ok := h.rooms[c.roomID]
	if !ok {
		return
	}

	delete(room.Peers, c.id)
	if len(room.Peers) == 0 {
		delete(h.rooms, room.ID)
		h.broadcastLocked(Envelope{
			Type:   "room_closed",
			RoomID: room.ID,
			SentAt: time.Now().UnixMilli(),
		}, room)
		h.broadcastRoomsLocked()
		return
	}

	if room.HostID == c.id {
		promoteRoomHost(room)
	}

	h.broadcastLocked(Envelope{
		Type:   "peer_left",
		RoomID: room.ID,
		PeerID: c.id,
		Nick:   c.nick,
		SentAt: time.Now().UnixMilli(),
	}, room)
	h.broadcastRoomsLocked()
}

func (h *Hub) handle(c *Client, env Envelope) {
	switch env.Type {
	case "hello":
		if env.Nick != "" {
			c.nick = env.Nick
		}
		h.enqueue(c, Envelope{Type: "hello", PeerID: c.id, SentAt: time.Now().UnixMilli()})
		h.sendRooms(c)
	case "list_rooms":
		h.sendRooms(c)
	case "create_room":
		h.createRoom(c, env)
	case "join_room":
		h.joinRoom(c, env)
	case "leave_room":
		h.leaveRoom(c)
	case "ice_offer", "ice_answer", "ice_candidate":
		h.routeToPeer(c, env)
	case "mute_status", "peer_status":
		h.broadcastPeerStatus(c, env)
	case "voice_frame":
		h.broadcastVoiceFrame(c, env)
	case "heartbeat":
		h.enqueue(c, Envelope{Type: "heartbeat_ack", SentAt: time.Now().UnixMilli()})
	default:
		h.enqueue(c, Envelope{Type: "error", Error: "unknown message type", SentAt: time.Now().UnixMilli()})
	}
}

func (h *Hub) createRoom(c *Client, env Envelope) {
	h.mu.Lock()
	defer h.mu.Unlock()

	if c.roomID != "" {
		h.leaveRoomLocked(c)
	}

	name := "Squad room"
	if env.Room != nil && env.Room.Name != "" {
		name = env.Room.Name
	}
	if env.Nick != "" {
		c.nick = env.Nick
	}

	room := &Room{
		ID:        "room_" + randomHex(8),
		Name:      name,
		HostID:    c.id,
		HostNick:  c.nick,
		Locked:    env.Room != nil && env.Room.Locked,
		CreatedAt: time.Now(),
		Peers:     map[string]*Client{c.id: c},
	}
	h.rooms[room.ID] = room
	c.roomID = room.ID

	snapshot := room.snapshot(true)
	h.enqueue(c, Envelope{Type: "room_created", RoomID: room.ID, Room: &snapshot, SentAt: time.Now().UnixMilli()})
	h.broadcastRoomsLocked()
}

func (h *Hub) joinRoom(c *Client, env Envelope) {
	h.mu.Lock()
	defer h.mu.Unlock()

	room, ok := h.rooms[env.RoomID]
	if !ok {
		h.enqueue(c, Envelope{Type: "error", Error: "room not found", SentAt: time.Now().UnixMilli()})
		return
	}
	if len(room.Peers) >= maxPeersPerRoom {
		h.enqueue(c, Envelope{Type: "error", Error: "room is full", SentAt: time.Now().UnixMilli()})
		return
	}
	if c.roomID != "" {
		h.leaveRoomLocked(c)
	}
	if env.Nick != "" {
		c.nick = env.Nick
	}

	room.Peers[c.id] = c
	c.roomID = room.ID

	snapshot := room.snapshot(true)
	h.enqueue(c, Envelope{Type: "joined_room", RoomID: room.ID, Room: &snapshot, SentAt: time.Now().UnixMilli()})
	h.broadcastLocked(Envelope{
		Type:    "peer_joined",
		RoomID:  room.ID,
		PeerID:  c.id,
		Nick:    c.nick,
		Payload: env.Payload,
		SentAt:  time.Now().UnixMilli(),
	}, room)
	h.broadcastRoomsLocked()
}

func (h *Hub) leaveRoom(c *Client) {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.leaveRoomLocked(c)
	h.broadcastRoomsLocked()
}

func (h *Hub) leaveRoomLocked(c *Client) {
	if c.roomID == "" {
		return
	}

	room, ok := h.rooms[c.roomID]
	c.roomID = ""
	if !ok {
		return
	}

	delete(room.Peers, c.id)
	if len(room.Peers) == 0 {
		delete(h.rooms, room.ID)
		h.broadcastLocked(Envelope{Type: "room_closed", RoomID: room.ID, SentAt: time.Now().UnixMilli()}, room)
		return
	}

	if room.HostID == c.id {
		promoteRoomHost(room)
	}

	h.broadcastLocked(Envelope{
		Type:   "peer_left",
		RoomID: room.ID,
		PeerID: c.id,
		Nick:   c.nick,
		SentAt: time.Now().UnixMilli(),
	}, room)
}

func (h *Hub) routeToPeer(c *Client, env Envelope) {
	h.mu.RLock()
	defer h.mu.RUnlock()

	target, ok := h.clients[env.TargetPeerID]
	if !ok || c.roomID == "" || target.roomID != c.roomID {
		h.enqueue(c, Envelope{Type: "error", Error: "target peer unavailable", SentAt: time.Now().UnixMilli()})
		return
	}

	env.PeerID = c.id
	env.Nick = c.nick
	env.RoomID = c.roomID
	env.SentAt = time.Now().UnixMilli()
	h.enqueue(target, env)
}

func (h *Hub) broadcastPeerStatus(c *Client, env Envelope) {
	h.mu.RLock()
	defer h.mu.RUnlock()

	room, ok := h.rooms[c.roomID]
	if !ok {
		return
	}
	env.PeerID = c.id
	env.Nick = c.nick
	env.RoomID = c.roomID
	env.SentAt = time.Now().UnixMilli()
	h.broadcastLocked(env, room)
}

func (h *Hub) broadcastVoiceFrame(c *Client, env Envelope) {
	h.mu.RLock()
	defer h.mu.RUnlock()

	room, ok := h.rooms[c.roomID]
	if !ok {
		return
	}
	if len(env.Payload) == 0 || len(env.Payload) > maxVoicePayloadBytes {
		return
	}

	env.Type = "voice_frame"
	env.PeerID = c.id
	env.Nick = c.nick
	env.RoomID = c.roomID
	env.TargetPeerID = ""
	env.SentAt = time.Now().UnixMilli()
	h.broadcastLocked(env, room)
}

func (h *Hub) sendRooms(c *Client) {
	h.mu.RLock()
	defer h.mu.RUnlock()
	h.enqueue(c, Envelope{Type: "rooms", Rooms: h.roomSnapshotsLocked(false), SentAt: time.Now().UnixMilli()})
}

func (h *Hub) broadcastRoomsLocked() {
	env := Envelope{Type: "rooms", Rooms: h.roomSnapshotsLocked(false), SentAt: time.Now().UnixMilli()}
	for _, client := range h.clients {
		h.enqueue(client, env)
	}
}

func (h *Hub) roomSnapshotsLocked(includePeers bool) []RoomSnapshot {
	rooms := make([]RoomSnapshot, 0, len(h.rooms))
	for _, room := range h.rooms {
		rooms = append(rooms, room.snapshot(includePeers))
	}
	return rooms
}

func (h *Hub) broadcastLocked(env Envelope, room *Room) {
	for peerID, peer := range room.Peers {
		if peerID == env.PeerID && env.Type != "room_closed" {
			continue
		}
		h.enqueue(peer, env)
	}
}

func (h *Hub) enqueue(c *Client, env Envelope) {
	select {
	case c.send <- env:
	default:
		log.Printf("dropping message for slow peer %s", c.id)
	}
}

func serveWS(h *Hub, w http.ResponseWriter, r *http.Request) {
	conn, err := websocket.Accept(w, r, &websocket.AcceptOptions{
		OriginPatterns: []string{"*"},
	})
	if err != nil {
		log.Printf("websocket accept: %v", err)
		return
	}

	ctx, cancel := context.WithCancel(r.Context())
	defer cancel()

	c := &Client{
		id:   "peer_" + randomHex(8),
		nick: "Player",
		conn: conn,
		send: make(chan Envelope, 64),
	}
	h.addClient(c)
	defer h.removeClient(c)

	go writeLoop(ctx, c)
	readLoop(ctx, h, c)
}

func readLoop(ctx context.Context, h *Hub, c *Client) {
	for {
		_, data, err := c.conn.Read(ctx)
		if err != nil {
			if !errors.Is(err, context.Canceled) {
				log.Printf("read peer %s: %v", c.id, err)
			}
			return
		}

		var env Envelope
		if err := json.Unmarshal(data, &env); err != nil {
			h.enqueue(c, Envelope{Type: "error", Error: "invalid json", SentAt: time.Now().UnixMilli()})
			continue
		}
		h.handle(c, env)
	}
}

func writeLoop(ctx context.Context, c *Client) {
	for {
		select {
		case <-ctx.Done():
			return
		case env := <-c.send:
			data, err := json.Marshal(env)
			if err != nil {
				log.Printf("marshal message for %s: %v", c.id, err)
				continue
			}

			writeCtx, cancel := context.WithTimeout(ctx, 5*time.Second)
			err = c.conn.Write(writeCtx, websocket.MessageText, data)
			cancel()
			if err != nil {
				log.Printf("write peer %s: %v", c.id, err)
				return
			}
		}
	}
}

func randomHex(bytes int) string {
	buf := make([]byte, bytes)
	if _, err := rand.Read(buf); err != nil {
		panic(err)
	}
	return hex.EncodeToString(buf)
}

func main() {
	hub := NewHub()
	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte("ok\n"))
	})
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte("Tarkov Voice Overlay coordinator\n"))
	})
	mux.HandleFunc("/ws", func(w http.ResponseWriter, r *http.Request) {
		serveWS(hub, w, r)
	})

	addr := os.Getenv("TVO_COORDINATOR_ADDR")
	if addr == "" {
		if port := os.Getenv("PORT"); port != "" {
			addr = ":" + port
		} else {
			addr = ":8080"
		}
	}

	log.Printf("tarkov voice coordinator listening on %s", addr)
	if err := http.ListenAndServe(addr, mux); err != nil {
		log.Fatal(err)
	}
}
