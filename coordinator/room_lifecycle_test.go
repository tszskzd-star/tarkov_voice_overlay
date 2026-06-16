package main

import (
	"encoding/json"
	"testing"
)

func TestRoomSurvivesHostLeavingUntilLastPeerLeaves(t *testing.T) {
	hub := NewHub()
	host := &Client{id: "peer_host", nick: "Host", send: make(chan Envelope, 16)}
	guest := &Client{id: "peer_guest", nick: "Guest", send: make(chan Envelope, 16)}
	hub.addClient(host)
	hub.addClient(guest)

	hub.handle(host, Envelope{
		Type: "create_room",
		Nick: "Host",
		Room: &RoomSnapshot{Name: "Lifecycle test"},
	})

	created := readQueuedEnvelope(t, host, "room_created")
	if created.Room == nil || created.Room.ID == "" {
		t.Fatalf("room_created did not include room id: %+v", created)
	}

	hub.handle(guest, Envelope{
		Type:   "join_room",
		RoomID: created.Room.ID,
		Nick:   "Guest",
	})
	_ = readQueuedEnvelope(t, guest, "joined_room")

	hub.leaveRoom(host)
	room := hub.rooms[created.Room.ID]
	if room == nil {
		t.Fatalf("room closed when host left but guest was still present")
	}
	if room.HostID != guest.id {
		t.Fatalf("host was not promoted to remaining peer: got %q want %q", room.HostID, guest.id)
	}
	if guest.roomID != created.Room.ID {
		t.Fatalf("guest left room unexpectedly: got %q want %q", guest.roomID, created.Room.ID)
	}

	hub.leaveRoom(guest)
	if _, ok := hub.rooms[created.Room.ID]; ok {
		t.Fatalf("room stayed open after last peer left")
	}
}

func readQueuedEnvelope(t *testing.T, client *Client, want string) Envelope {
	t.Helper()
	for i := 0; i < cap(client.send); i++ {
		select {
		case env := <-client.send:
			if env.Type == want {
				return env
			}
		default:
			t.Fatalf("did not receive %s; queued=%s", want, dumpQueued(client))
		}
	}
	t.Fatalf("did not receive %s", want)
	return Envelope{}
}

func dumpQueued(client *Client) string {
	pending := make([]Envelope, 0, len(client.send))
	for {
		select {
		case env := <-client.send:
			pending = append(pending, env)
		default:
			data, _ := json.Marshal(pending)
			return string(data)
		}
	}
}
