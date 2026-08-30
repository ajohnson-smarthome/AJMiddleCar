#!/bin/bash
# The full Mac acceptance run for Plan 1: everything the plan promised, plus the
# regression that cost a session's connectivity to find.
#
# The two halves run in one pass, not two scripts, because they answer the same
# question from opposite directions: "1. THE PLAN'S DELIVERABLE" below checks that
# the dongle does what it's for (host gets an address, dongle answers, /status
# answers); "2. THE REGRESSION THAT MUST NOT RETURN" checks that it does nothing
# else — no captured default route, no `router` DHCP option, no added latency. A
# build that passes only the first half is the build that shipped `gw = 192.168.7.1`
# and cost a Mac its internet (`docs/research/2026-08-21-usb-ethernet-dongle.md`,
# "Донгл обязан отказаться быть шлюзом"); a build that only passes the second half
# never proved the dongle does anything at all. Keep them together so neither can
# regress unnoticed while the other stays green.
#
# Usage: ./verify-on-host.sh <output-file>
# Plug the dongle in any time after starting — the script polls for its interface.
OUT="$1"
: > "$OUT"

# The LAN router used for the Wi-Fi-latency baseline. This is this bench's router,
# not a project constant — override it for a different LAN:
#   LAN_ROUTER=192.168.1.1 ./verify-on-host.sh out.txt
LAN_ROUTER="${LAN_ROUTER:-192.168.2.1}"

echo "########## BASELINE — no dongle ##########" >> "$OUT" 2>&1
{
  echo "route get 1.1.1.1:"; route -n get 1.1.1.1 2>&1 | grep -E "gateway|interface" | sed 's/^/  /'
  echo "internet:"; curl -s -o /dev/null -w "  by IP: HTTP %{http_code} in %{time_total}s\n" --max-time 5 https://1.1.1.1
  curl -s -o /dev/null -w "  by name: HTTP %{http_code} in %{time_total}s\n" --max-time 6 https://api.github.com
  echo "wifi latency:"; ping -c 4 -W 1000 -t 6 "$LAN_ROUTER" 2>&1 | tail -1 | sed 's/^/  /'
} >> "$OUT" 2>&1

for i in $(seq 1 300); do
  IF=""
  for n in $(ifconfig -l | tr ' ' '\n' | grep -E '^en[0-9]+$'); do
    if ifconfig "$n" 2>/dev/null | grep -q "inet 192.168.7"; then IF="$n"; break; fi
  done
  if [ -n "$IF" ]; then
    sleep 3
    {
      echo
      echo "########## DONGLE ATTACHED as $IF ##########"
      echo
      echo "=== 1. THE PLAN'S DELIVERABLE ==="
      echo "host address from the dongle's DHCP server:"
      ifconfig "$IF" | grep "inet " | sed 's/^/  /'
      echo "dongle answers ping:"
      ping -c 3 -W 1000 -t 4 192.168.7.1 2>&1 | tail -2 | sed 's/^/  /'
      echo "GET /status:"
      curl -s --max-time 5 -w "\n  [HTTP %{http_code} in %{time_total}s]\n" http://192.168.7.1:8080/status 2>&1 | sed 's/^/  /'
      echo
      echo "=== 2. THE REGRESSION THAT MUST NOT RETURN ==="
      echo "route get 1.1.1.1 (must be unchanged from baseline):"
      route -n get 1.1.1.1 2>&1 | grep -E "gateway|interface" | sed 's/^/  /'
      echo "DHCP options the host actually received (must have NO router):"
      ipconfig getpacket "$IF" 2>&1 | sed -n '/^options:/,/^end/p' | sed 's/^/  /'
      echo "host internet, with the dongle attached:"
      curl -s -o /dev/null -w "  by IP: HTTP %{http_code} in %{time_total}s\n" --max-time 5 https://1.1.1.1
      curl -s -o /dev/null -w "  by name: HTTP %{http_code} in %{time_total}s\n" --max-time 6 https://api.github.com
      echo "wifi latency (must match baseline):"
      ping -c 4 -W 1000 -t 6 "$LAN_ROUTER" 2>&1 | tail -1 | sed 's/^/  /'
      echo "dongle traffic counters (a quiet device, not a chatty one):"
      netstat -ibn -I "$IF" 2>/dev/null | tail -1 | sed 's/^/  /'
    } >> "$OUT" 2>&1
    break
  fi
  sleep 1
done
echo "VERIFY-DONE" >> "$OUT"
