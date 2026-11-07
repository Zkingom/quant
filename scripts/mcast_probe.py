#!/usr/bin/env python3
"""Loopback UDP multicast probe (L5.0): does this Linux / WSL2 host support
what L5 needs, and how much does its kernel buffer hold?

    python3 -I scripts/mcast_probe.py [--iface 127.0.0.1] [--runs 3]

1. Environment: kernel, WSL networking mode (if any), loopback flags, rmem limits.
2. A/B probe (repeated --runs times): two receivers join two groups on the
   interface; a sender with IP_MULTICAST_LOOP=1 / IP_MULTICAST_IF=<iface>
   sends 200 datagrams to each group. Counts what arrives and whether in order.
   Then a unicast reply to the source address of a received datagram must
   reach the sender (the MoldUDP64 retransmission request/response path).
3. Burst: 100 000 datagrams of 1 008 bytes while nobody reads, then drain.
   Shows the kernel receive-buffer limit; one more datagram afterwards carries
   the kernel's cumulative drop counter (SO_RXQ_OVFL), checking that counter.

Receives use a 0.5 s timeout only to bound a failed probe; no result depends
on timing. Exit status 0 iff every A/B probe received everything.
"""
import argparse
import os
import platform
import socket
import struct
import subprocess

N = 200
GROUPS = [("239.255.0.1", 31001), ("239.255.0.2", 31002)]
SO_RXQ_OVFL = 40  # linux/socket.h


def sh(cmd):
    try:
        return subprocess.run(cmd, shell=True, capture_output=True, text=True, check=False).stdout.strip()
    except OSError as e:
        return f"({e})"


def environment():
    print("== environment")
    print("kernel:", platform.release())
    wslconfig = sh("ls /mnt/c/Users/*/.wslconfig 2>/dev/null | head -1")
    if wslconfig:
        mode = sh(f"grep -i networkingMode '{wslconfig}'") or "networkingMode not set (NAT default)"
        print("wsl:", mode)
    print("lo:", sh("ip link show lo | head -1"))
    for key in ("net.core.rmem_default", "net.core.rmem_max"):
        print(key, "=", sh(f"sysctl -n {key}"))


def join(group, port, iface):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", port))
    mreq = struct.pack("4s4s", socket.inet_aton(group), socket.inet_aton(iface))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
    s.settimeout(0.5)
    return s


def sender(iface):
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    tx.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 0)
    tx.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
    tx.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(iface))
    tx.bind((iface, 0))
    return tx


def ab_probe(iface):
    try:
        rx = [join(g, p, iface) for g, p in GROUPS]
        tx = sender(iface)
    except OSError as e:
        print(f"  setup failed: {e}")
        return False
    for i in range(N):
        for g, p in GROUPS:
            tx.sendto(struct.pack(">Q", i) + b"x" * 40, (g, p))
    ok = True
    src = None
    for (g, p), s in zip(GROUPS, rx):
        got = []
        try:
            while len(got) < N:
                data, src = s.recvfrom(2048)
                got.append(struct.unpack(">Q", data[:8])[0])
        except socket.timeout:
            pass
        print(f"  group {g}:{p}: {len(got)}/{N} received, in order: {got == list(range(len(got)))}")
        ok = ok and len(got) == N
    if src is not None:
        rx[0].sendto(b"reply", src)
        tx.settimeout(0.5)
        try:
            tx.recvfrom(64)
            print(f"  unicast reply to the sender {src}: received")
        except socket.timeout:
            print(f"  unicast reply to the sender {src}: NOT received")
            ok = False
    for s in rx + [tx]:
        s.close()
    return ok


def burst(iface, n=100_000, size=1008):
    g, p = "239.255.0.9", 31009
    rx = join(g, p, iface)
    rx.setsockopt(socket.SOL_SOCKET, SO_RXQ_OVFL, 1)
    tx = sender(iface)
    for i in range(n):
        tx.sendto(struct.pack(">Q", i) + b"y" * (size - 8), (g, p))
    got = 0
    try:
        while True:
            rx.recvmsg(2048, 64)
            got += 1
    except socket.timeout:
        pass
    tx.sendto(b"probe", (g, p))  # carries the cumulative drop counter
    _, anc, _, _ = rx.recvmsg(2048, 64)
    drops = next((struct.unpack("I", d[:4])[0] for _, t, d in anc if t == SO_RXQ_OVFL), 0)
    print(f"  SO_RCVBUF {rx.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)} B; burst of {n} x {size} B with no reader: "
          f"{got} received, {n - got} lost; kernel drop counter (SO_RXQ_OVFL) = {drops}")
    rx.close()
    tx.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iface", default="127.0.0.1")
    ap.add_argument("--runs", type=int, default=3)
    args = ap.parse_args()
    environment()
    results = []
    for r in range(args.runs):
        print(f"== A/B probe on {args.iface}, run {r + 1}")
        results.append(ab_probe(args.iface))
    print(f"== burst on {args.iface}")
    burst(args.iface)
    print("== result:", "multicast usable" if all(results) else "multicast NOT usable", f"({sum(results)}/{len(results)} runs)")
    raise SystemExit(0 if all(results) else 1)


if __name__ == "__main__":
    if os.name != "posix":
        raise SystemExit("Linux only")
    main()
