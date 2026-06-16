package main

import (
	"context"
	"encoding/json"
	"os"
	"testing"
	"time"

	"nhooyr.io/websocket"
)

func TestRenderSignalsAndVoiceFallback(t *testing.T) {
	url := os.Getenv("TVO_RENDER_WS")
	if url == "" {
		t.Skip("set TVO_RENDER_WS to run the live Render signaling check")
	}

	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()

	a := mustDialTestClient(t, ctx, url)
	defer a.Close(websocket.StatusNormalClosure, "done")
	b := mustDialTestClient(t, ctx, url)
	defer b.Close(websocket.StatusNormalClosure, "done")

	mustWriteEnvelope(t, ctx, a, Envelope{Type: "hello", Nick: "VoiceTestA"})
	mustWriteEnvelope(t, ctx, b, Envelope{Type: "hello", Nick: "VoiceTestB"})
	helloA := mustReadType(t, ctx, a, "hello")
	helloB := mustReadType(t, ctx, b, "hello")
	if helloA.PeerID == "" || helloB.PeerID == "" {
		t.Fatalf("hello did not assign peer ids: a=%+v b=%+v", helloA, helloB)
	}

	mustWriteEnvelope(t, ctx, a, Envelope{
		Type: "create_room",
		Nick: "VoiceTestA",
		Room: &RoomSnapshot{Name: "Voice relay test", Locked: false},
	})
	created := mustReadType(t, ctx, a, "room_created")
	if created.Room == nil || created.Room.ID == "" {
		t.Fatalf("room_created did not include a room id: %+v", created)
	}

	mustWriteEnvelope(t, ctx, b, Envelope{
		Type:   "join_room",
		RoomID: created.Room.ID,
		Nick:   "VoiceTestB",
	})
	mustReadType(t, ctx, b, "joined_room")

	mustWriteEnvelope(t, ctx, a, Envelope{
		Type:         "ice_offer",
		RoomID:       created.Room.ID,
		TargetPeerID: helloB.PeerID,
		Payload:      json.RawMessage(`{"transport":"udp","candidates":[{"type":"host","address":"127.0.0.1","port":41000}]}`),
	})

	offer := mustReadType(t, ctx, b, "ice_offer")
	if offer.PeerID != helloA.PeerID {
		t.Fatalf("ice_offer did not include source peer id: got %q want %q", offer.PeerID, helloA.PeerID)
	}
	if offer.TargetPeerID != helloB.PeerID {
		t.Fatalf("ice_offer changed target peer id: got %q want %q", offer.TargetPeerID, helloB.PeerID)
	}

	mustWriteEnvelope(t, ctx, a, Envelope{
		Type:    "voice_frame",
		Payload: json.RawMessage(`{"sequence":42,"captureUnixMs":123,"audio":"AAECAw=="}`),
	})
	relayed := mustReadType(t, ctx, b, "voice_frame")
	if relayed.PeerID != helloA.PeerID {
		t.Fatalf("voice_frame did not include source peer id: got %q want %q", relayed.PeerID, helloA.PeerID)
	}
	if string(relayed.Payload) == "" {
		t.Fatalf("voice_frame did not include payload: %+v", relayed)
	}
}

func mustDialTestClient(t *testing.T, ctx context.Context, url string) *websocket.Conn {
	t.Helper()
	conn, _, err := websocket.Dial(ctx, url, nil)
	if err != nil {
		t.Fatalf("dial %s: %v", url, err)
	}
	return conn
}

func mustWriteEnvelope(t *testing.T, ctx context.Context, conn *websocket.Conn, env Envelope) {
	t.Helper()
	payload, err := json.Marshal(env)
	if err != nil {
		t.Fatalf("marshal envelope: %v", err)
	}
	if err := conn.Write(ctx, websocket.MessageText, payload); err != nil {
		t.Fatalf("write envelope: %v", err)
	}
}

func mustReadType(t *testing.T, ctx context.Context, conn *websocket.Conn, want string) Envelope {
	t.Helper()
	for {
		_, payload, err := conn.Read(ctx)
		if err != nil {
			t.Fatalf("waiting for %s: %v", want, err)
		}
		var env Envelope
		if err := json.Unmarshal(payload, &env); err != nil {
			t.Fatalf("unmarshal envelope: %v", err)
		}
		if env.Type == want {
			return env
		}
	}
}
