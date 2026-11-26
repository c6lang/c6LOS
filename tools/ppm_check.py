#!/usr/bin/env python3
"""校验 P6 PPM：尺寸 + 若干像素（可带容差）。"""
import argparse
import sys

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", required=True)
    ap.add_argument("--min-w", type=int, default=1)
    ap.add_argument("--min-h", type=int, default=1)
    ap.add_argument("--pixel", action="append", default=[],
                    help="x,y,r,g,b[,tol]")
    ap.add_argument("--nonblank", action="store_true")
    a = ap.parse_args()
    try:
        f = open(a.file, "rb")
    except OSError as e:
        print("ppm: %s" % e, file=sys.stderr)
        return 2
    if f.readline().strip() != b"P6":
        print("ppm: not P6", file=sys.stderr)
        return 2
    line = f.readline()
    while line.startswith(b"#"):
        line = f.readline()
    w, h = map(int, line.split())
    maxv = int(f.readline())
    if maxv != 255:
        print("ppm: maxval %d unsupported" % maxv, file=sys.stderr)
        return 2
    if w < a.min_w or h < a.min_h:
        print("FAIL: ppm size %dx%d < %dx%d" % (w, h, a.min_w, a.min_h))
        return 1
    data = f.read(w * h * 3)
    if len(data) != w * h * 3:
        print("FAIL: ppm truncated")
        return 1
    if a.nonblank:
        if len(set(data)) <= 1:
            print("FAIL: ppm is a single color")
            return 1
    for spec in a.pixel:
        parts = [int(x) for x in spec.split(",")]
        x, y, r, g, b = parts[:5]
        tol = parts[5] if len(parts) > 5 else 2
        if not (0 <= x < w and 0 <= y < h):
            print("FAIL: pixel %d,%d outside %dx%d" % (x, y, w, h))
            return 1
        o = (y * w + x) * 3
        got = (data[o], data[o + 1], data[o + 2])
        if any(abs(got[i] - (r, g, b)[i]) > tol for i in range(3)):
            print("FAIL: pixel %d,%d got %s want %s" % (x, y, got, (r, g, b)))
            return 1
    print("PASS: ppm %dx%d" % (w, h))
    return 0

if __name__ == "__main__":
    sys.exit(main())
