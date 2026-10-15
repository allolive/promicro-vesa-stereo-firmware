#!/usr/bin/env python3
"""Read the light rig (lightrig.ino) and report, per sensor: the switching
period, how long it stays bright, and where its edges sit relative to the
reference sensor's rising edge (by default sensor 0, the top of the screen).

Usage:
  lightrig.py [--port /dev/ttyACM0] [--ref 0] [--seconds 5]
  lightrig.py --raw CH [--samples 900]     one raw trace of one sensor

Typical setups (TEMT6000 modules on A0..A3):
  draw time   A0 at the top of the screen, A1 at the bottom, playing a clip that
              is white in one eye and black in the other with 3D on: the lag of
              sensor 1's rising edge behind sensor 0's is the drawing time.
  glasses     A2 / A3 behind the left / right lens, looking at the same white
              patch as A0: when each lens opens and closes against the screen.
"""

import argparse
import os
import statistics
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                                "resources", "lib"))
from shutter3d.engine import open_serial  # noqa: E402

WRAP = 1 << 32          # the rig's timer, in 0.5 us ticks


def open_port(path):
    return os.fdopen(open_serial(path, nonblock=False), "r+b", buffering=0)


def lines(port, until):
    buf = b""
    while time.time() < until:
        chunk = port.read(256)
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            ln, buf = buf.split(b"\n", 1)
            yield ln.decode("ascii", "replace").strip()


def summarise(edges, ref):
    """edges: {ch: [(ticks, rising)]}, 0.5 us ticks; returns printable lines."""
    out = []
    ref_rise = [t for t, r in edges.get(ref, []) if r]
    for c in sorted(edges):
        ev = edges[c]
        rises = [t for t, r in ev if r]
        falls = [t for t, r in ev if not r]
        if len(rises) < 3:
            out.append("sensor %d: %d edges, not switching" % (c, len(ev)))
            continue
        periods = [(b - a) % WRAP / 2 for a, b in zip(rises, rises[1:], strict=False)]
        p = statistics.median(periods)
        highs = []
        for t in rises:
            nxt = [(f - t) % WRAP / 2 for f in falls if 0 < (f - t) % WRAP / 2 < p]
            if nxt:
                highs.append(min(nxt))
        line = "sensor %d: period %.1f us (%.3f Hz), bright %.1f us (%.0f%%)" % (
            c, p, 1e6 / p, statistics.median(highs) if highs else 0,
            100 * statistics.median(highs) / p if highs else 0)
        if c != ref and ref_rise:
            lags = []
            for t in rises:
                lag = [(t - r) % WRAP / 2 for r in ref_rise if (t - r) % WRAP / 2 < p]
                if lag:
                    lags.append(min(lag))
            if lags:
                line += ", rises %.1f us after sensor %d (spread %.1f us)" % (
                    statistics.median(lags), ref, max(lags) - min(lags))
        out.append(line)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--ref", type=int, default=0)
    ap.add_argument("--seconds", type=float, default=5)
    ap.add_argument("--raw", type=int)
    ap.add_argument("--samples", type=int, default=900)
    args = ap.parse_args()

    port = open_port(args.port)
    if args.raw is not None:
        port.write(b"M 0\nR %d %d\n" % (args.raw, args.samples))
        for ln in lines(port, time.time() + 5):
            if ln.startswith("R "):
                f = ln.split()
                us = float(f[2])
                vals = [int(v) for v in f[3:]]
                print("sensor %s, %.2f us per sample" % (f[1], us))
                for i in range(0, len(vals), max(1, len(vals) // 60)):
                    print("%8.1f us %4d %s" % (i * us, vals[i], "#" * (vals[i] // 4)))
                break
        port.write(b"M 1\n")
        return

    port.write(b"M 1\n")
    edges = {}
    for ln in lines(port, time.time() + args.seconds):
        f = ln.split()
        if len(f) == 4 and f[0] == "E":
            edges.setdefault(int(f[1]), []).append((int(f[3], 16), f[2] == "1"))
    if not edges:
        print("no edges: check the sensors face something that switches", file=sys.stderr)
        sys.exit(1)
    print("\n".join(summarise(edges, args.ref)))


if __name__ == "__main__":
    main()
