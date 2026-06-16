# Tarkov Voice Overlay

This folder is a separate Windows project added next to the existing Aurora NFC
application. It implements the project shape for the requested voice overlay:

- C++20 client core with room, peer, mute, VAD, overlay, hotkey, signaling, and
  ICE configuration boundaries.
- Go WebSocket coordinator for public room listing and ICE/message exchange.
- Direct UDP voice transport in the client: the coordinator carries signaling
  only and rejects voice frames.
- STUN preset: `stun:stun.l.google.com:19302`.
- Metered/Open Relay TURN integration point. Metered currently issues usable
  TURN credentials through an account/API flow, so real production credentials
  must be inserted at build/deploy time.
- LAN fallback and native media backends are isolated behind interfaces so the
  UI and protocol state do not need to change when they are wired in.

## Layout

```text
tarkov_voice_overlay/
  client/        C++20 client core and native backend boundaries
  coordinator/   Go WebSocket room/ICE coordinator
  docs/          build and protocol notes
```

## Client build

Install Visual Studio 2022, CMake, and vcpkg. Then from this folder:

```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:\path\to\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
```

The default build keeps native audio/overlay/network backends disabled so the
core can be smoke-tested early. Enable full backend wiring with:

```powershell
cmake -S . -B build-native -DTVO_ENABLE_NATIVE_BACKENDS=ON -DCMAKE_TOOLCHAIN_FILE=C:\path\to\vcpkg\scripts\buildsystems\vcpkg.cmake
```

## Coordinator build

Install Go 1.22+ and run:

```powershell
cd coordinator
go mod tidy
go run .
```

By default it listens on `:8080`, with WebSocket endpoint `/ws` and health
endpoint `/healthz`.

## Current state

This is a solid implementation scaffold and a functional coordinator, not a
finished six-hour production voice client yet. The remaining production work is
listed in `docs/build-and-acceptance.md`.
