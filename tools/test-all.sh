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

echo "== swift host tests =="
for dir in app/tests/*/; do
    name="$(basename "$dir")"
    swiftc -o "/tmp/hosttest_$name" app/AJMiddleCar/Generated/CarAPI.swift "$dir/main.swift"
    "/tmp/hosttest_$name"
done

echo "== all green =="
