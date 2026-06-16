#include "tvo/core/VoiceSession.h"
#include "tvo/security/PasswordGate.h"

#include <cassert>
#include <string>

int main() {
    tvo::VoiceSession session;

    tvo::RoomInfo room;
    room.id = "room_test";
    room.name = "Test";
    room.maxPeers = tvo::kMaxPeersPerRoom;

    assert(session.createRoom(room, "host", "Host"));
    assert(session.inRoom());
    assert(session.peers().size() == 1);
    assert(session.hasCapacityForPeer());

    for (int i = 0; i < 4; ++i) {
        tvo::PeerStatus peer;
        peer.id = "peer_" + std::to_string(i);
        peer.nick = "Peer";
        session.addOrUpdatePeer(peer);
    }

    assert(session.peers().size() == tvo::kMaxPeersPerRoom);
    assert(!session.hasCapacityForPeer());

    session.setLocalMicLevel(0.7f);
    assert(session.localPeer().speaking);

    session.toggleLocalMute();
    assert(session.localPeer().muted);
    assert(!session.localPeer().speaking);

    tvo::PasswordGate gate;
    gate.setPassword("secret");
    const auto proof = gate.makeJoinProof("room_test", "peer_a", "nonce");
    assert(gate.verifyJoinProof("room_test", "peer_a", "nonce", proof));
    assert(!gate.verifyJoinProof("room_test", "peer_a", "bad", proof));

    return 0;
}
