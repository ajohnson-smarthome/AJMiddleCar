#!/usr/bin/env bash
# Build and flash the ESP32-C6 radio co-processor. One-time bench procedure — see README.md.
#
#   firmware/c6/flash-radio.sh                 # build only, then print the flash command
#   firmware/c6/flash-radio.sh /dev/cu.usbXXX  # build and flash through the C6 UART header
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HOSTED="$ROOT/firmware/p4/managed_components/espressif__esp_hosted"
CP="$HOSTED/examples/wifi/sta/cp"

if [ ! -d "$CP" ]; then
    echo "esp_hosted is not fetched yet — it arrives with the host build. Run:"
    echo "  (cd firmware/p4 && source ../../tools/env-p4.sh && idf.py reconfigure)"
    exit 1
fi

# shellcheck disable=SC1091
source "$ROOT/tools/env-p4.sh" >/dev/null 2>&1

# The co-processor's SDIO datapath sends frames larger than stock ESP-IDF allows. This is the
# vendor's own patch and it is idempotent; without it the build stops with an explicit error.
python "$HOSTED/tools/eh.py" patch-idf --idf-path "$IDF_PATH" >/dev/null

cd "$CP"
[ -d build ] || idf.py set-target esp32c6 >/dev/null
idf.py build

if [ $# -ge 1 ]; then
    idf.py -p "$1" flash monitor
else
    echo
    echo "Built. Connect a USB-serial adapter to the board's ESP32-C6 UART header, then:"
    echo "  firmware/c6/flash-radio.sh /dev/cu.usbserial-XXXX"
fi
