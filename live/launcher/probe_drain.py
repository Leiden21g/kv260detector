#!/usr/bin/env python3
# probe_drain.py — devmem が無い board 向けの probedrain(root で実行)。probe FIFO(axi_fifo_mm_s @0x80010000)の
#   RDFR(0x18) に 0xA5 を 1 秒毎に書いて RX FIFO を flush する(= probe_drain_daemon.sh と同じ動作)。
import mmap, os, sys, time, struct
BASE=0x80010000; RDFR=0x18; INTERVAL=float(os.environ.get("INTERVAL","1"))
fd=os.open("/dev/mem", os.O_RDWR|os.O_SYNC)
m=mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ|mmap.PROT_WRITE, offset=BASE)
print(f"[probe-drain] python start interval={INTERVAL}s", flush=True)
while True:
    m[RDFR:RDFR+4]=struct.pack("<I",0xA5)
    time.sleep(INTERVAL)
