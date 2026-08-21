# Plan — pinning the car's traffic to Wi-Fi

Spec: `docs/superpowers/specs/2026-08-21-wifi-pinned-networking.md`

App-only. No firmware change, no protocol change, no release needed.

## Task 1 — the shared pinning rule

`app/AJMiddleCar/CarNet.swift`

- `CarNet.params(webSocket: Bool) -> NWParameters` — TCP parameters, `requiredInterfaceType = .wifi`
  and `prohibitedInterfaceTypes = [.cellular]` on device, neither in the simulator (loopback is not
  Wi-Fi and the mock car would become unreachable).
- `CarNet.endpoint(port:)` — host/port from `CarHost`, so the address stays defined in one place.

Nothing else may set `requiredInterfaceType`; both transports go through here.

## Task 2 — the response parser, pure and tested

`app/AJMiddleCar/HTTPParse.swift`

- `HTTPParse.head(_ bytes: [UInt8]) -> Head?` where `Head` carries `status`, `contentLength`,
  `bodyOffset`.
- Handles: status line, headers to the blank line, case-insensitive `Content-Length`, absent
  `Content-Length`, an incomplete buffer (returns nil, caller reads more), a malformed status line.

Host test `app/tests/test_httpparse.swift` run with `swiftc`, matching how `ControlModel` and
`Tricks` are tested. Must pass before Task 3 uses it.

## Task 3 — the HTTP client

`app/AJMiddleCar/CarHTTP.swift`

- `get(_ path: String, timeout: TimeInterval) async -> (status: Int, body: Data)?`
- `post(_ path: String, body: Data, contentType: String, timeout: TimeInterval) async -> (status: Int, body: Data)?`
- One `NWConnection` per request, `Connection: close`, read until `Content-Length` is satisfied or
  the stream ends. `.waiting` past the deadline is a failure, not a wait — that state is the whole
  reason this change exists.

## Task 4 — the control socket

Rewrite `app/AJMiddleCar/CarConnection.swift` onto `NWConnection` + `NWProtocolWebSocket.Options`.

Keep the existing surface exactly — `start`, `pause`, `resume`, `setCommand`, `onTelemetry`,
`state` — so no view changes. Keep the 10 Hz send timer and the reconnect-on-failure behaviour.

While here, fix what the bench found and leave documented: `start()` guarded on `started` so it
only ever connected once per process.

## Task 5 — move the clients across

Swap `URLSession` for `CarHTTP` in: `CarStatus` (bootstrap), `CalibClient`, `RampClient`,
`TrimClient`, `RecoverClient`, `WheelClient`, `DimsClient`, and `UpdateClient.upload` (the `/ota`
POST only).

**`UpdateClient`'s GitHub calls stay on `URLSession`** — internet probe, release lookup, firmware
download. Pinning those to Wi-Fi would point them at a car that has no internet.

## Task 6 — verify

1. Host tests: `swiftc` parser test passes.
2. Simulator: mock car still reachable — proves the simulator escape hatch works.
3. Device: install, open the app, wait past the point where `path[any]` goes unsatisfied, drive.
   Telemetry must keep arriving and the car's log must stop showing `wdt: no control frame`.
4. Remove `DiagProbe.swift` and the `DIAG` logging before committing.

## Order

1 → 2 → 3 → 4 → 5 → 6. Tasks 3 and 4 both depend on 1; 5 depends on 3.
