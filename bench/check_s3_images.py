#!/usr/bin/env python3
"""S3 镜像静态预检：入口/向量/段/栈边界，不依赖硬件。"""
import re
import subprocess
import sys

XT = "/home/winger/.c6os/toolchain/xtensa-esp-elf/bin/xtensa-esp32s3-elf-"
FILES = [
    ("c6LOS default", "build/s3/c6os.elf", 0x403C9000),
    ("c6LOS perf", "build/s3perf/c6os.elf", 0x403C9000),
    ("c6LOS rt", "build/s3rt/c6os.elf", 0x403C9000),
    ("LiteOS-M bench", "bench/liteos_m_s3/build/liteos_s3.elf", 0x403C9000),
]


def run(*args):
    return subprocess.run(args, capture_output=True, text=True, check=True).stdout


def sym(elf, name):
    out = run(XT + "nm", elf)
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[2] == name:
            return int(p[0], 16)
    return None


def main():
    bad = 0
    for label, elf, vec_expect in FILES:
        try:
            entry = int(run(XT + "readelf", "-h", elf).split("Entry point address:")[1].split()[0], 16)
            init = sym(elf, "_init_start")
            if init is None:
                init = sym(elf, "_vecbase")
            bss_end = sym(elf, "_bss_end")
            if bss_end is None:
                bss_end = sym(elf, "_ebss")
            sec = run(XT + "readelf", "-S", elf)
            m = re.search(r"\.iram0\.vectors\s+\S+\s+([0-9a-f]+)", sec)
            if m is None:
                m = re.search(r"\.vector\s+PROGBITS\s+([0-9a-f]+)", sec)
            vec = int(m.group(1), 16) if m else None
            ok = entry is not None and init == vec_expect and vec == vec_expect
            if bss_end is not None and bss_end + 0x800 > 0x3FCEF000:
                ok = False
            print(f"{label:16s} entry=0x{entry:08x} init=0x{init:08x} vec=0x{vec:08x} bss_end=0x{bss_end:08x} {'OK' if ok else 'BAD'}")
            if not ok:
                bad += 1
        except Exception as e:
            print(f"{label:16s} FAIL {e}")
            bad += 1
    print("PASS: s3 static preflight" if bad == 0 else f"FAIL: {bad} image(s)")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
