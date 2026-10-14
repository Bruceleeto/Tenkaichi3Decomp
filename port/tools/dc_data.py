#!/usr/bin/env python3
"""The game's data tables for the Dreamcast build: gen_data.py's assembly (port/build/gen/data) assembled with
sh-elf-as into OUT (Makefile.dc). SH ELF names C symbols with a leading underscore, so every symbol the files
define or refer to (.globl, labels, the operands of .long) gets one; strings and numbers are left as they are.

    port/tools/dc_data.py OUT_DIR"""
import pathlib, re, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = ROOT / "port/build/gen/data"
IDENT = re.compile(r"(?<![\w.$])([A-Za-z_][A-Za-z0-9_]*)")
# the PS2 C library's tables the game's data holds, which KOS's C library (newlib) defines too, with the same
# layout: the data's copy gives way to the library's
LIBC = {"_ctype_"}


def convert(text):
    out = []
    for line in text.splitlines():
        s = line.strip()
        if s.startswith(".globl "):
            name = s[7:].strip()
            line = (".weak _" if name in LIBC else ".globl _") + name
        elif re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*:", s):
            line = "_" + s
        elif s.startswith(".long "):
            line = "    .long " + IDENT.sub(r"_\1", s[6:])
        out.append(line)
    return "\n".join(out) + "\n"


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    dst = pathlib.Path(sys.argv[1])
    dst.mkdir(parents=True, exist_ok=True)
    for old in dst.glob("*.o"):
        old.unlink()
    for f in sorted(SRC.glob("*.s")):
        s = dst / f.name
        s.write_text(convert(f.read_text()))
        r = subprocess.run(["sh-elf-as", "--little", str(s), "-o", str(s.with_suffix(".o"))], capture_output=True, text=True)
        if r.returncode:
            sys.exit(f"{f.name}: {r.stderr[:400]}")
    print(f"{len(list(dst.glob('*.o')))} data objects assembled for the Dreamcast")


if __name__ == "__main__":
    main()
