#!/usr/bin/env python3
"""Minimal mock of the ESP32-Car firmware HTTP/WS API for running the iOS app
in the simulator without hardware. Serves /status, /ws, /calib*, /ramp, /trim, /wheel,
and /ota on 127.0.0.1:8080."""
import asyncio
import json
import os
import time
from aiohttp import web, WSMsgType

START = time.monotonic()
STATE = {"calibrated": False, "ramp_ms": 300, "trim_pct": 0, "wdt_trips": 0,
         "wheel": {"diameter_mm": 65, "ppr": 11, "gear_x100": 2100, "quad": 4},
         "dims": {"track_mm": 130, "wheelbase_mm": 210},
         "recover": {"enabled": False, "window_ms": 3000}}


async def status(request):
    return web.json_response({
        # The mock exists to be indistinguishable from the car at the protocol level, so it
        # carries the same identity the firmware reports — otherwise the app's wrong-car check
        # would reject it. Override via MOCK_DEVICE to exercise that path.
        "device": os.environ.get("MOCK_DEVICE", "ajmiddlecar"),
        "fw": "v1.0+9000",
        "uptime_s": int(time.monotonic() - START),
        "calibrated": STATE["calibrated"],
        "heap": 200000,
        "rssi": -58,
        "ws_fps": 10,
        "wdt_trips": STATE["wdt_trips"],
        "radio": {"fw": "mock", "expected": "mock", "ok": True},
    })


async def ws(request):
    wsr = web.WebSocketResponse()
    await wsr.prepare(request)
    print("ws: client connected")

    async def pusher():
        while not wsr.closed:
            payload = {
                "rssi": -58,
                "ws_fps": 10,
                "wdt_trips": STATE.get("wdt_trips", 0),
                "uptime_s": int(time.monotonic() - START),
                "heap": 200000,
                "calibrated": STATE.get("calibrated", False),
            }
            try:
                await wsr.send_str(json.dumps(payload))
            except Exception:
                break
            await asyncio.sleep(0.2)  # 5 Hz

    push_task = asyncio.create_task(pusher())
    try:
        async for msg in wsr:
            if msg.type == WSMsgType.TEXT:
                try:
                    f = json.loads(msg.data)
                    print(f"ws rx: t={f['t']} y={f['y']}")
                except Exception:
                    print(f"ws rx (bad json): {msg.data}")
            elif msg.type == WSMsgType.ERROR:
                print(f"ws error: {wsr.exception()}")
    finally:
        push_task.cancel()
    print("ws: client disconnected")
    return wsr


async def calib(request):
    return web.json_response({"calibrated": STATE["calibrated"]})


async def calib_spin(request):
    try:
        b = await request.json()
        pair, d = int(b["pair"]), int(b["dir"])
        if not (0 <= pair <= 3 and d in (0, 1)):  # mirror the firmware's range check
            raise ValueError
        print(f"calib spin: pair={pair} dir={d}")
    except Exception:
        return web.Response(status=400, text="need {pair:0..3,dir:0|1}")
    return web.Response(text="ok")


async def calib_save(request):
    try:
        wheels = (await request.json())["wheels"]
        assert isinstance(wheels, list) and len(wheels) == 4
        # mirror calibration_valid: 4 unique pairs 0..3, signs ±1
        pairs = {int(w["pair"]) for w in wheels}
        assert pairs == {0, 1, 2, 3}
        assert all(int(w["sign"]) in (-1, 1) for w in wheels)
    except Exception:
        return web.Response(status=400, text="need {wheels:[4x{pair:0..3,sign:±1}]}")
    STATE["calibrated"] = True
    print(f"calib/save: calibrated=True wheels={wheels}")
    return web.Response(text="ok")


async def ramp_get(request):
    return web.json_response({"ramp_ms": STATE["ramp_ms"]})


async def ramp_post(request):
    try:
        v = int((await request.json())["ramp_ms"])
        if not (0 <= v <= 2000):
            raise ValueError
    except Exception:
        return web.Response(status=400, text="need {ramp_ms}")
    STATE["ramp_ms"] = v
    print(f"ramp_ms: {v}")
    return web.Response(text="ok")


async def trim_get(request):
    return web.json_response({"trim_pct": STATE["trim_pct"]})


async def trim_post(request):
    try:
        v = int((await request.json())["trim_pct"])
        if not (-30 <= v <= 30):
            raise ValueError
    except Exception:
        return web.Response(status=400, text="need {trim_pct}")
    STATE["trim_pct"] = v
    print(f"trim_pct: {v}")
    return web.Response(text="ok")


async def wheel_get(request):
    return web.json_response(STATE["wheel"])


async def wheel_post(request):
    try:
        b = await request.json()
        d, ppr, gear, quad = b["diameter_mm"], b["ppr"], b["gear_x100"], b["quad"]
        if not (20 <= d <= 150 and 1 <= ppr <= 1000 and 100 <= gear <= 30000 and quad in (1, 2, 4)):
            raise ValueError
    except Exception:
        return web.Response(status=400, text="need {diameter_mm,ppr,gear_x100,quad}")
    STATE["wheel"] = {"diameter_mm": d, "ppr": ppr, "gear_x100": gear, "quad": quad}
    print(f"wheel: {STATE['wheel']}")
    return web.Response(text="ok")


async def dims_get(request):
    return web.json_response(STATE["dims"])


async def dims_post(request):
    try:
        b = await request.json()
        track, base = b["track_mm"], b["wheelbase_mm"]
        if not (60 <= track <= 300 and 90 <= base <= 360):
            raise ValueError
    except Exception:
        return web.Response(status=400, text="need {track_mm,wheelbase_mm}")
    STATE["dims"] = {"track_mm": track, "wheelbase_mm": base}
    print(f"dims: {STATE['dims']}")
    return web.Response(text="ok")


async def recover_get(request):
    return web.json_response(STATE["recover"])


async def recover_post(request):
    try:
        b = await request.json()
        en, win = b["enabled"], int(b["window_ms"])
        if not isinstance(en, bool):   # firmware needs a JSON bool (cJSON_IsBool), not 0/1
            raise ValueError
        if not (1000 <= win <= 10000):
            raise ValueError
    except Exception:
        return web.Response(status=400, text="need {enabled,window_ms}")
    STATE["recover"] = {"enabled": en, "window_ms": win}
    print(f"recover: {STATE['recover']}")
    return web.Response(text="ok")


async def ota(request):
    data = await request.read()
    print(f"ota: received {len(data)} bytes")
    return web.Response(text="ok")


def main():
    app = web.Application()
    app.add_routes([
        web.get("/status", status),
        web.get("/ws", ws),
        web.get("/calib", calib),
        web.post("/calib/spin", calib_spin),
        web.post("/calib/save", calib_save),
        web.post("/ota", ota),
        web.get("/ramp", ramp_get),
        web.post("/ramp", ramp_post),
        web.get("/trim", trim_get),
        web.post("/trim", trim_post),
        web.get("/wheel", wheel_get),
        web.post("/wheel", wheel_post),
        web.get("/dims", dims_get),
        web.post("/dims", dims_post),
        web.get("/recover", recover_get),
        web.post("/recover", recover_post),
    ])
    print("mock car on http://127.0.0.1:8080  (/status, /ws, /calib*, /ramp, /trim, /recover, /wheel, /dims, /ota)")
    web.run_app(app, host="127.0.0.1", port=8080)


if __name__ == "__main__":
    main()
