#!/usr/bin/env python3
# Finalize a GoldHEN plugin PRX from the create-fself OELF output.
# GoldHEN loads ELF-prx with ET_SCE_DYNLIB (0xFE18) and NO section headers
# (matches every known-working plugin: shoff=shentsize=shnum=shstrndx = 0).
# usage: python fix_prx.py <in.oelf> <out.prx>
import struct
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: fix_prx.py <in.oelf> <out.prx>")
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, "rb") as fh:
        b = bytearray(fh.read())
    if bytes(b[:4]) != b"\x7fELF":
        print(f"{src}: not an ELF")
        return 1
    etype = struct.unpack_from("<H", b, 0x10)[0]
    if etype != 0xFE18:
        print(f"{src}: unexpected e_type 0x{etype:x} (want 0xfe18 - is this really an OELF?)")
        return 1
    struct.pack_into("<Q", b, 0x28, 0)   # e_shoff
    struct.pack_into("<H", b, 0x3A, 0)   # e_shentsize
    struct.pack_into("<H", b, 0x3C, 0)   # e_shnum
    struct.pack_into("<H", b, 0x3E, 0)   # e_shstrndx
    with open(dst, "wb") as fh:
        fh.write(b)
    print(f"[fix_prx] {dst} ({len(b)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
