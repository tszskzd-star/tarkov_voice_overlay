# Build and acceptance notes

## Target behavior

- User starts the client and sees public rooms or creates one.
- No IP fields or manual network codes are exposed.
- Coordinator carries room metadata and ICE/signaling messages only.
- Voice frames flow peer-to-peer over the direct UDP transport negotiated with
  `ice_offer`/`ice_answer` signaling. The coordinator rejects `voice_frame`.
- Room password is verified locally. The coordinator never receives it.
- Overlay is top-most, click-through, translucent, and does not steal focus.
- Global mute hotkey updates local capture immediately and broadcasts status.

## Metered TURN

Metered's current Open Relay documentation says TURN credentials are obtained by
signing up and calling their REST API to get an `iceServers` array. The code
keeps this as a build/deploy-time credential source instead of inventing fake
credentials.

Recommended production flow:

1. Create a Metered account and TURN credential/API key.
2. Put the app name/API key in a private build secret.
3. Bake the resolved TURN server list into the release, or let the coordinator
   fetch short-lived credentials and sign them for clients.
4. Keep `stun:stun.l.google.com:19302` as the default STUN server.

## Native backend checklist

- Audio:
  - PortAudio capture/playback at 48 kHz mono.
  - RNNoise before VAD/Opus encode. The current vcpkg RNNoise port is not
    available for regular Windows builds, so vendor/build RNNoise from source
    for the native client instead of making it a mandatory vcpkg dependency.
  - Opus 20 ms frames, constrained VBR, low delay, packet loss concealment.
  - Jitter buffer per remote peer.
- Network:
  - WebSocket coordinator reconnect/backoff.
  - The current client has a direct UDP voice transport with host candidates and
    best-effort STUN server-reflexive candidates.
  - libnice/full ICE and TURN relay fallback for symmetric NAT.
  - Per-peer encrypted data channel or UDP stream.
- Security:
  - libsodium Argon2id room key from password and room salt.
  - X25519 key exchange per peer.
  - AEAD encryption for app-level control and voice frames.
- Windows UI:
  - Dear ImGui + DirectX 11 main window.
  - Layered transparent click-through overlay window.
  - Shell tray icon and global hotkeys.

## Acceptance tests to add before release

- Two local clients can create/join a room through the coordinator.
- Five clients sustain Opus/RNNoise processing for six hours without leaks.
- TURN-only mode connects through symmetric NAT.
- LAN discovery works when the coordinator cannot be reached.
- Overlay remains click-through and keeps focus in Escape from Tarkov.
- Idle CPU stays under 0.5%; five-peer voice stays under 2% on target hardware.
