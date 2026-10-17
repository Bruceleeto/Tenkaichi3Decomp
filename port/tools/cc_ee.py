#!/usr/bin/env python3
"""One file of the 32-bit gcc build with the PS2 compiler's float constants, for the Makefile: what
undefined.py's compile_ee does, plus a dependency file for make.

    port/tools/cc_ee.py [--game] OUT.o SRC -- gcc ...flags...

--game: a game source (src/): the C of its functions that do not match yet is enabled first (portsrc.prepare).
--dc-inline HEADER: (the Dreamcast) each call of a routine HEADER has an inline copy of (dcv_<name>) is made a call
of the copy; a declaration or definition of it (its name after a type) and an address taken are left as they are.
The preprocessor writes OUT.d (the headers SRC includes); eeconst rewrites the float constants of the
preprocessed text (OUT.i), which is then compiled."""
import pathlib, re, subprocess, sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import eeconst
import portsrc


NOT_TYPES = {"return", "else", "do", "case", "sizeof", "goto"}


def dc_inline(text, header):
    """text with each call of a routine the header copies made a call of the copy (dcv_<name>)"""
    names = sorted(set(re.findall(r"\bdcv_([A-Z]\w*)\s*\(", header.read_text())))
    call = re.compile(r"(?<![\w.>])(" + "|".join(names) + r")(\s*\()")

    def one(m):
        before = re.search(r"(\w+)\s*$", text[max(0, m.start() - 64):m.start()])
        if before and before.group(1) not in NOT_TYPES:  # after a type (or a qualifier): a declaration
            return m.group(0)
        return "dcv_" + m.group(1) + m.group(2)

    # a call through the routine cast to its type first (a game macro's: ((void (*)(Vec4 *dst, Vec4 *src))Vec4_Copy)(..))
    cast = re.compile(r"\(\(\w+\s*\(\*\)\([^()]*\)\)(" + "|".join(names) + r")\)(\s*\()")
    text = cast.sub(lambda m: "dcv_" + m.group(1) + m.group(2), text)
    return call.sub(one, text)


def main():
    args = sys.argv[1:]
    game = args[:1] == ["--game"]
    if game:
        args = args[1:]
    inline = None
    if args[:1] == ["--dc-inline"]:
        inline, args = pathlib.Path(args[1]), args[2:]
    out, src, sep, cmd = pathlib.Path(args[0]), pathlib.Path(args[1]), args[2], args[3:]
    if sep != "--" or not cmd:
        sys.exit(__doc__)
    if game:
        src, _, missing = portsrc.prepare(src.resolve())
        if missing:
            print(f"no C for: {src.name} {' '.join(missing)}", file=sys.stderr)
    pre = [a for a in cmd if a != "-c"] + ["-E", "-MMD", "-MP", "-MF", str(out.with_suffix(".d")), "-MT", str(out), str(src)]
    r = subprocess.run(pre, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stderr)
        return r.returncode
    i = out.with_suffix(".i")
    text = eeconst.transform(r.stdout)
    if inline is not None:
        text = dc_inline(text, inline)
    i.write_text(text)
    r = subprocess.run(cmd + ["-fpreprocessed", str(i), "-o", str(out)])
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
