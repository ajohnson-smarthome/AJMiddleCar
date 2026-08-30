#!/usr/bin/env bash
# Every host test in the project, plus the contract drift check. No hardware,
# no simulator, no ESP-IDF — this is what runs before every commit.
#
# The XCTest bundle in app/AJMiddleCarTests is deliberately NOT run here: it
# needs a simulator. Pure Swift belongs in app/tests/<name>/main.swift instead.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "== contract =="
python3 tools/test_gen_contract.py
bash tools/check_contract.sh

echo "== firmware host tests =="
make -C firmware/p4/test run
make -C firmware/s3/test run

echo "== swift host tests =="
for dir in app/tests/*/; do
    name="$(basename "$dir")"
    # A test that exercises an app module lists it, one path per line, in `sources` next to
    # main.swift — the alternative is copying the module into the test, which tests the copy.
    extra=()
    if [ -f "${dir}sources" ]; then
        while read -r src; do
            [ -n "$src" ] && extra+=("app/AJMiddleCar/$src")
        done < "${dir}sources"
    fi
    swiftc -o "/tmp/hosttest_$name" app/AJMiddleCar/Generated/CarAPI.swift \
        ${extra[@]+"${extra[@]}"} "${dir}main.swift"
    "/tmp/hosttest_$name"
done

echo "== mock host tests =="
python3 tools/mock_car/test_state.py
python3 tools/mock_car/test_rtlink.py

echo "== conformance =="
# The REST matrix needs a running mock, which needs aiohttp, which needs the venv. A
# missing venv is a skip rather than a failure so a fresh clone can run everything else —
# but set CONFORMANCE=required (CI, or a pre-flash check) and the skip becomes an error,
# so "nobody has run it in months" cannot look the same as "it passed".
MOCK_PY="tools/mock_car/.venv/bin/python"
if [ ! -x "$MOCK_PY" ]; then
    echo "skipped: tools/mock_car/.venv is missing — the mock needs aiohttp. Create it with"
    echo "  python3 -m venv tools/mock_car/.venv"
    echo "  tools/mock_car/.venv/bin/pip install -r tools/mock_car/requirements.txt"
    if [ "${CONFORMANCE:-}" = "required" ]; then
        echo "CONFORMANCE=required, and it did not run" >&2
        exit 1
    fi
else
    # Spare ports, on loopback: a mock already serving the simulator keeps the contract's
    # ports, and this one only has to answer REST.
    PORT=8137
    RT_PORT=4237
    LOG="$(mktemp -t mockcar)"
    "$MOCK_PY" tools/mock_car/mock_car.py --host 127.0.0.1 --port "$PORT" \
        --rt-port "$RT_PORT" > "$LOG" 2>&1 &
    MOCK_PID=$!
    trap 'kill "$MOCK_PID" 2>/dev/null || true; rm -f "$LOG"' EXIT

    ready=0
    for _ in $(seq 1 50); do
        if python3 -c "import urllib.request as u; u.urlopen('http://127.0.0.1:$PORT/status', timeout=1)" 2>/dev/null; then
            ready=1
            break
        fi
        sleep 0.1
    done
    if [ "$ready" -ne 1 ]; then
        echo "the mock did not come up:" >&2
        cat "$LOG" >&2
        exit 1
    fi

    python3 tools/conformance.py "http://127.0.0.1:$PORT"
    # Latent coupling: if conformance.py ever grows a valid-image OTA case, note
    # that the mock's simulated reboot (rt_link.py's REBOOT_QUIET_S, 4 s) outlasts
    # this tool's ~3 s hello-retry budget — a run started right after would see
    # "unreachable" instead of the fresh post-reboot handshake.
    python3 tools/conformance_rt.py "127.0.0.1:$RT_PORT"
    kill "$MOCK_PID" 2>/dev/null || true
    wait "$MOCK_PID" 2>/dev/null || true
    rm -f "$LOG"
    trap - EXIT
fi

echo "== all green =="
