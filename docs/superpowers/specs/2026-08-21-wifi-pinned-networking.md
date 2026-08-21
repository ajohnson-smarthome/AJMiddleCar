# Pinning the car's traffic to Wi-Fi

## The problem, as measured

The car is a stand-alone Wi-Fi accessory: it publishes an access point and never provides a route
to the internet. iOS does not accept such a network at face value. It probes for internet access,
and when the probe fails it stops treating the interface as usable for general traffic — while
Settings still says "connected".

Instrumenting the app on the bench (2026-08-21) caught the transition exactly:

```
14:44:46  path[wifi-only]: satisfied   wifi=yes
14:44:46  path[any]:       satisfied   wifi=yes cell=no
14:44:46  pinned TCP to 192.168.4.1:80 — READY in 21 ms
14:44:47  URLSession GET /status — 200, 177 bytes

14:45:30  path[wifi-only]: satisfied   wifi=yes          <- Wi-Fi still usable
14:45:30  path[any]:       unsatisfied wifi=no cell=yes  <- general path gone
14:45:33  URLSession GET /status — NSURLErrorTimedOut
```

Forty seconds after launch the *general* path goes unsatisfied and drops Wi-Fi, while a path
restricted to Wi-Fi stays satisfied. `URLSession` runs on the general path and therefore stops
reaching the car; a connection bound to the Wi-Fi interface reaches it in 21 ms.

This matches Apple's own description in [The iOS Wi-Fi
Lifecycle](https://developer.apple.com/forums/thread/734361): a network that fails the internet
check keeps its interface up but is **prevented from becoming the default route**.

The user-visible symptom was a car that could be driven for a few seconds after launch and then
went unreachable, with the app falling back to its "searching" screen.

## What this changes

**All traffic addressed to the car moves from `URLSession` to `Network.framework`, with the
connection bound to the Wi-Fi interface.** `URLSession` cannot express that binding — it is not a
setting we failed to switch on, it is a capability the API does not have. `NWParameters` does:

```swift
params.requiredInterfaceType = .wifi
```

Everything else keeps working as it does today.

## What this does not change

**Traffic to GitHub stays on `URLSession`.** The internet probe, the release lookup and the
firmware download must use whatever path actually reaches the internet — usually cellular, and
specifically *not* the car's Wi-Fi. Pinning those would break them. The split is by destination:
the car is pinned, the internet is not.

**The wire protocol is untouched.** No firmware change, no new endpoints, no version bump on the
car. This is entirely a change in how the phone opens its sockets.

**The app's public behaviour is untouched.** `CarConnection` keeps its interface (`start`,
`pause`, `resume`, `setCommand`, `onTelemetry`), so the views do not change.

## Rejected: answering the captive-network probe on the car

The first attempt was firmware-side: a DNS responder for `*.apple.com` plus a page satisfying
Apple's reachability probe, so that iOS would consider the network fit for general traffic. It was
written, host-tested, flashed, and **it did not fix the problem** — the general path went
unsatisfied anyway.

It is also the wrong shape. Apple's [Working with a Wi-Fi
Accessory](https://developer.apple.com/forums/thread/734344) advises against a stand-alone
accessory behaving as a captive network, and the approach only ever aimed to change iOS's verdict
rather than to stop depending on it. Reverted.

## Design

### Two pieces, one rule

- **`CarHTTP`** — a minimal HTTP/1.1 client over `NWConnection` for the REST endpoints.
- **`CarSocket`** — the `/ws` control and telemetry socket over `NWConnection` with
  `NWProtocolWebSocket`.

Both build their `NWParameters` through one shared helper so the pinning rule lives in a single
place and cannot drift between them.

### The simulator must not be pinned

`CarHost` points the simulator at `127.0.0.1:8080`, where the mock car runs. Loopback is not
Wi-Fi, so pinning there would break every simulator build. The helper applies
`requiredInterfaceType` only off-simulator.

### HTTP, deliberately minimal

One connection per request, `Connection: close`, and a response reader that uses `Content-Length`
when present and end-of-stream otherwise. The car's endpoints are small and cold-path — every REST
call is a settings screen or a one-shot probe, while the 10 Hz control stream rides the socket.
Connection reuse would buy nothing and cost state to manage.

The exception is `POST /ota`, which carries the firmware image (~765 KB). It is the same code path
with a large body; `NWConnection.send` handles the framing.

### The parser is pure, and tested

Splitting a response into status line, headers and body is the only part with real edge cases, so
it is a pure function with no `Network` dependency, host-tested with `swiftc` the way the project's
other pure modules are. Everything else is I/O glue that only a device can exercise.

### Failure and timeouts

`NWConnection` can sit in `.waiting` indefinitely when a path is unavailable — which is exactly
the state this change exists to survive. Each request therefore carries its own deadline and
treats `.waiting` past it as a failure, so a stalled path surfaces as an error instead of a hang.
The socket keeps today's reconnect behaviour.

## How we will know it worked

The bench check is the symptom that started this: open the app, wait past the forty-second mark
where the general path collapsed, and drive. Previously the car became unreachable; now the
telemetry keeps arriving and `wdt: no control frame` stops appearing in the car's log.

The `path[any]: unsatisfied` line is expected to keep appearing. That is the point — the app no
longer depends on that path.
