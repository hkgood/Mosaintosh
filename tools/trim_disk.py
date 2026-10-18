#!/usr/bin/env python3
"""Make a smaller copy of an HFS system disk so it fits the board's flash.

Deletes the given paths (colon-separated, from the volume root), then writes
a new volume of the requested size. The source's boot blocks are copied over
the new ones; without them the ROM has nothing to jump into.

usage: trim_disk.py in.img out.img size_mb [path ...]
   eg: trim_disk.py System.img Small.img 10 ":HyperCard 2.4"

Needs machfs (pip install machfs).
"""

import sys

import machfs


def remove(vol, path):
    parts = path.strip(":").split(":")
    folder = vol
    for part in parts[:-1]:
        folder = folder[part]
    del folder[parts[-1]]


def volume_name(raw):
    """drVN from the master directory block; machfs does not read it back."""
    mdb = 1024
    n = raw[mdb + 0x24]
    return raw[mdb + 0x25:mdb + 0x25 + n].decode("mac_roman")


def main():
    src, dst, size_mb, paths = sys.argv[1], sys.argv[2], float(sys.argv[3]), sys.argv[4:]
    raw = open(src, "rb").read()
    if raw[0:2] != b"LK":
        sys.exit(f"{src}: no boot blocks, not a startup disk")

    vol = machfs.Volume()
    vol.read(raw)
    vol.name = volume_name(raw)
    for p in paths:
        try:
            remove(vol, p)
        except KeyError:
            sys.exit(f"{p}: not on the volume")
        print(f"removed {p}")

    size = int(size_mb * 1024 * 1024) // 512 * 512
    out = bytearray(vol.write(size=size, align=512, desktopdb=True, bootable=True))
    out[0:1024] = raw[0:1024]
    open(dst, "wb").write(out)
    print(f"{dst}: {len(out) / 1048576:.1f} MB")


if __name__ == "__main__":
    main()
