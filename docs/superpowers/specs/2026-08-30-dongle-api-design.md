# The app ↔ dongle API

The contract between `app/` and `firmware/s3/`. Like `docs/protocol.md` for the car, this
document and the dongle's own behaviour are the whole seam: neither side references the other in
code, and either should be reimplementable from this file alone.

Plan 1 established that iOS accepts the dongle as an ordinary network interface
(`firmware/s3/README.md`, verified 2026-08-30). This spec covers what the two then say to each
other.

## What the dongle is, and is not

**A modem, not a brain.** It knows no car: no SSID, no protocol, no device id compiled in. It is
told which network to join at runtime and carries bytes it does not interpret. That rule is the
same one `firmware/c6/` lives under, and it is what lets AJPicoCar use the same dongle with no
change to either.

Everything the app does with the car today — the five config domains, calibration, OTA,
telemetry, driving — passes through unchanged. The dongle adds no feature to that traffic and
subtracts none. It does not cache the car's settings, does not stage the car's firmware, and has
no opinion about what a `/wheel` is.

**The dongle replaces the direct Wi-Fi path.** The app no longer joins the car's softAP; the
dongle does. The phone keeps its own Wi-Fi and cellular throughout, which is the entire point of
the device.

## Two contracts that do not know each other

```
contract/car-api.json      app ↔ car     (exists; unchanged by this spec)
this document              app ↔ dongle  (new)
```

The app is the only place the two meet. It reads `CarContract.ssid` and `CarContract.password`
and hands them to the dongle as opaque strings; the dongle never learns whose they are.

**The dongle's API is hand-written, not generated.** `tools/gen_contract.py` earns its keep on
the car's five config domains — ten fields with ranges, four artifacts, a drift check. The
dongle has one config domain with two fields. Generalising the generator (its output routing is
four string literals in `main()`, duplicated again in `check_contract.sh`) would cost more than
it saves.

There is ample precedent: six of the car's eleven HTTP endpoints — `/`, `/status`, `/calib`,
`/calib/spin`, `/calib/save`, `/ota` — are hand-written in both firmware and docs, and are
first-class parts of the protocol. If the dongle's surface ever grows to justify generation, the
schema can be introduced then; nothing here forecloses it.

**Conventions are borrowed wholesale from the car**, because consistency across the two devices
is worth more than any local optimum:

- Everything is JSON, `Content-Type: application/json`.
- A successful POST returns `{"ok":true}`.
- A rejection returns `4xx` with `{"error":"…","field":"…"}`; `field` is empty when the body as a
  whole is at fault.
- Values are **rejected, never clamped**.
- Configuration persists in NVS as one JSON string per domain, with a dirty check so an unchanged
  POST does not rewrite flash.
- The identity key is `device`, matching the car's `device_field`. (Plan 1 shipped `dev`; this
  spec renames it. The endpoint has been verified on a bench and nowhere else, so the moment to
  make the two devices agree is now.)

## Addressing: how the app reaches the car

### Why network address translation is required, not preferred

The obvious design — give the phone a route to `192.168.4.0/24` via the dongle — fails, and it
fails at the car's end.

The car is a softAP with no uplink. Its routing table holds its own subnet and nothing else. A
packet arriving from `192.168.7.2` is processed, but the reply has nowhere to go: there is no
route to that address and no default gateway to fall back on. Traffic would flow one way and
die.

Two fixes exist. Add a route to the car's firmware — which breaks "the car does not change", and
would have to be repeated for AJPicoCar. Or make the car see the traffic as coming **from the
dongle**, at an address in its own subnet, which it can answer on-link. The second is NAT, and it
is the only one that keeps the car untouched.

ESP-IDF 6.0.2 supports it natively: `CONFIG_LWIP_IP_FORWARD`, `CONFIG_LWIP_IPV4_NAPT`,
`CONFIG_LWIP_IPV4_NAPT_PORTMAP`, with `ip_napt_enable_netif()` and `ip_portmap_add()`.

### Why the route is not pushed by DHCP

DHCP option 121 (classless static route) would let the dongle hand the phone a route to the
car's subnet without touching the default route. IDF's DHCP server cannot send it: `add_offer_options`
emits nine options, and 121 is not among them. Using it would mean patching lwIP.

Recorded because it is the obvious next idea, and it is unavailable rather than unwise.

### The port map

Static mappings on the dongle's own address. The phone needs no route at all — `192.168.7.1` is
on-link.

| Reached at | Goes to | Carries |
|---|---|---|
| `192.168.7.1:80` | the car's `:80` | the car's whole REST surface |
| `192.168.7.1:4210` UDP | the car's `:4210` | the real-time channel |
| `192.168.7.1:8080` | the dongle itself | this document's API |

**The car keeps its native ports; the dongle takes the unusual one.** Not symmetry for its own
sake: it is what leaves the most code untouched. `CarHost.port` stays `80`, `CarHost.rtPort`
stays `4210`, and `contract/car-api.json` does not change at all. Only `CarHost.host` moves. The
dongle is the new thing in the system, so the dongle absorbs the strangeness.

The real-time channel survives translation without special handling. The app streams commands at
10 Hz; the car sees the dongle as the sender and pushes telemetry back to it at 5 Hz; the
translation is refreshed continuously from both directions and cannot lapse mid-drive.

## Two things this changes about what Plan 1 shipped

Both are in code that has run on hardware, so neither is free — and both are cheaper now than
they will ever be again.

**The dongle's HTTP server moves from `:80` to `:8080`.** It has to: `:80` is being given to the
car, and the two cannot both have it. `status_api.c` sets the port in one line.

**`GET /status` renames `dev` to `device`.** The car's contract calls that field `device`, and the
app's "which device am I talking to" check should not need two spellings.

Nothing outside `firmware/s3/` consumes either yet — no app code, no script, and the one
verification tool (`firmware/s3/verify-on-host.sh`) is ours to update in the same commit.

## The dongle's API

Three paths on `192.168.7.1:8080`.

### `GET /status` — identity and radio state

```json
{
  "device": "ajdongle",
  "fw": "v1.0+483",
  "usb": "up",
  "net": {
    "ssid": "AJMiddleCar",
    "state": "connected",
    "rssi": -58
  }
}
```

| Field | Meaning |
|---|---|
| `device` | Always `ajdongle`. How the app tells this apart from any other USB-Ethernet adapter the user might plug in. |
| `fw` | The dongle's firmware version. |
| `usb` | `up` whenever this response was served, which is tautological but keeps the shape stable if a future state is added. |
| `net.ssid` | The network the dongle is configured to join. Empty string when unconfigured. |
| `net.state` | `idle` \| `joining` \| `connected` \| `failed`. |
| `net.rssi` | Signal strength in dBm, or `0` when not connected. |

`net.rssi` is worth more than the car's own. The car reports `rssi: 0` whenever its AP station
list is unavailable — on the P4 that is an RPC over SDIO to a co-processor, so "unavailable" is
ordinary, and `ControlModel.signalLevel` carries a fallback for it. The dongle is a station and
reads its own receiver, always. The signal bars stop being an estimate.

There is no field naming where the car is. Its address is a constant of this contract, not a
runtime discovery — `net.state` answers the only question that varies, which is whether the car
is reachable at all.

### `GET /net` — what network am I set to join

```json
{"ssid": "AJMiddleCar", "configured": true}
```

**Never returns the password.** The app has no use for reading it back — it holds the value
itself, in `CarContract` — and an endpoint that returns a stored credential is a liability with
no compensating benefit. `configured` is false and `ssid` empty before the first `POST`.

### `POST /net` — join this network

```json
{"ssid": "AJMiddleCar", "password": "drive1234"}
```

Answers `{"ok":true}`, persists to NVS, and begins joining. The response does not wait for the
join to succeed: `GET /status` reports `net.state` for that. A `POST` whose body matches the
stored value does not rewrite flash and does not restart the radio.

Validation, rejecting rather than clamping:

| Field | Rule | On failure |
|---|---|---|
| `ssid` | 1–32 bytes | `400 {"error":"ssid must be 1..32 bytes","field":"ssid"}` |
| `password` | empty, or 8–63 bytes | `400 {"error":"password must be empty or 8..63 bytes","field":"password"}` |

The bounds are WPA2's, not ours: 32 bytes is the maximum SSID length, and a PSK shorter than 8
characters cannot be used. An empty password means an open network, which the car's
`identity.h` already contemplates.

Both fields are required. A body missing either, or carrying an unknown key, is rejected whole —
the car's domains behave the same way.

### `POST /ota` — update the dongle

A raw application image in a single request, mirroring the car's `/ota` exactly: reject bodies
under 4 KB, reject anything whose first byte is not the ESP image magic, time out a stalled
transfer, and reboot into the new slot on success.

The flash is already partitioned for this — two 4 MB slots and `otadata`, laid down in Plan 1
before any OTA code existed, because repartitioning a device that lives in a pocket costs
physical access.

`UpdateClient.swift` is reused as-is. It already speaks this shape.

## The configuration surface must not be reachable over the radio

**This is a requirement, not a description.** Nothing enforces it today, and the code says so.

`httpd_start` binds `INADDR_ANY`, and `httpd_config_t` in IDF 6.0.2 has no bind-address field.
The dongle's API is USB-only right now for one reason: no other network interface exists. The
moment the radio comes up, the same server answers on the car's network — and by then `POST /net`
carries a Wi-Fi password.

The plan that brings up the station must therefore also bring the guard. Two mechanisms are
available:

- `getsockname()` on `httpd_req_to_sockfd(req)`, rejecting any request whose local address is not
  `192.168.7.1`; or
- `httpd_config_t.open_fn`, rejecting at accept time before a request is parsed.

The second is preferable — it refuses the connection rather than the request — but either
satisfies the requirement. `firmware/s3/main/status_api.h` already carries this warning at the
declaration a future author will read first.

## What changes in the app

Small, and concentrated.

| File | Change |
|---|---|
| `CarHost.swift` | `host` becomes the dongle's address rather than `CarContract.host`. `port` and `rtPort` are unchanged. |
| The transport's `NWParameters` | `requiredInterfaceType` moves from `.wifi` to `.wiredEthernet` (`docs/superpowers/specs/2026-08-21-wifi-pinned-networking.md` established the binding; only the interface type changes). |
| `ConnectView.swift` | "Join network X with password Y" becomes "plug in the dongle". The dongle's own states — absent, present but not configured, joining, cannot find the car — replace the Wi-Fi ones. |
| `LinkState.swift` | The state machine gains the dongle's states. Today it distinguishes "no Wi-Fi" from "connected"; the dongle can additionally be present but idle, or connected to the wrong network. |
| A new dongle client | `GET /status`, `GET`/`POST /net`, `POST /ota` against `192.168.7.1:8080`. Small, and modelled on `CalibClient.swift`. |
| `ControlModel.signalLevel` | May take RSSI from the dongle instead of the car. Optional; the existing fallback keeps working either way. |

**Unchanged:** `ConfigStore`, `RTFrame`, `CarTransport`'s framing, every settings screen, the
trick editor, the calibration wizard. They address the car through `CarHost` and speak the car's
contract; neither moves.

**The simulator is unchanged too.** `CarHost` already branches on `targetEnvironment(simulator)`
to reach the mock at `127.0.0.1`. There is no USB in the simulator and no dongle to mock: the
simulator keeps talking to `tools/mock_car` directly, exactly as today. Only real-device builds
go through the dongle. Dongle-specific screens are exercised on hardware, or against a stub if
one proves necessary — deliberately not built in advance.

## Configuration is automatic

The user never types an SSID. The app holds `CarContract.ssid` and `CarContract.password` and
sends them itself: on seeing a dongle whose `GET /net` reports a different network, or none, it
POSTs the right one. Switching between the two cars means launching the other app, which
reconfigures the dongle in one request.

This is better than what it replaces. Today, changing cars means leaving the app, opening
Settings, and joining a different Wi-Fi network by hand.

## What this spec does not decide

- **The order of implementation.** Config before radio, radio before the app — argued in
  `docs/superpowers/plans/2026-08-29-dongle-p1-ncm-endpoint.md` under "Why this plan stops where
  it does" — but the plans themselves are written separately.
- **Whether the dongle's own API stays on `:8080`.** It is a contract constant, changeable in one
  place on each side for as long as nothing else has been built against it.
- **Video.** Out of scope. If it arrives it will want its own port, and the port map has room.

## Open risks

**`ip_portmap_add`'s behaviour with the real-time channel is reasoned, not measured.** The
argument above — that a continuous bidirectional flow keeps the translation alive — is sound, but
the car pushes telemetry to an address it learned through NAT, and that path deserves a bench
test before the app depends on it. Symptom to watch for: telemetry stops while commands keep
working.

**The radio's arrival is where the image size question lands.** The app is 395 KB today against a
4 MB slot; the station and its stack will roughly double it. Comfortable, and worth measuring
rather than assuming, because the earlier 1 MB partition would have been tight.
