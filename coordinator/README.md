# Coordinator

Lightweight WebSocket coordinator for rooms and ICE signaling.

It does not forward voice. Voice must go peer-to-peer through the native ICE
transport.

## Run

```powershell
go mod tidy
go run .
```

Environment:

```text
TVO_COORDINATOR_ADDR=:8080
```

Endpoints:

- `GET /healthz`
- `WS /ws`

Production notes:

- Put it behind TLS (`wss://`) before distributing the client.
- Keep room passwords out of the coordinator. Only public keys, salts, proofs,
  ICE offers, answers, and candidates should pass through it.
- Add rate limits before public Internet deployment.

