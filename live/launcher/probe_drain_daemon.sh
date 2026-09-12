#!/bin/bash
# probe_drain_daemon.sh — probe FIFO (axi_fifo_mm_s @0x80010000) を定期 RDFR reset して
#   re-arm wedge を恒久回避する (reboot 不要)。boot probe が 16word/invocation 蓄積 → 512 満杯 → wedge。
#   RDFR(0x80010018)<-0xA5 で RX FIFO flush。連続 e2e は probe を読まないので flush は無害。
RDFR=0x80010018; RDFO=0x8001001C
INTERVAL=${INTERVAL:-1}
command -v devmem >/dev/null || exec sudo python3 "$(dirname "$0")/probe_drain.py"   # devmem 無し(EDF 25.11 等)は python 版へ
echo "[probe-drain] start interval=${INTERVAL}s"
while true; do
  occ=$(sudo devmem $RDFO 32 2>/dev/null)
  sudo devmem $RDFR 32 0x000000A5 2>/dev/null
  sleep "$INTERVAL"
done
