#!/usr/bin/env python3
"""One file of the 32-bit gcc build with the PS2 compiler's float constants, for the Makefile: what
undefined.py's compile_ee does, plus a dependency file for make.

    port/tools/cc_ee.py [--game] OUT.o SRC -- gcc ...flags...

--game: a game source (src/): the C of its functions that do not match yet is enabled first (portsrc.prepare).
The preprocessor writes OUT.d (the headers SRC includes); eeconst rewrites the float constants of the
preprocessed text (OUT.i), which is then compiled."""
import pathlib, subprocess, sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import eeconst
import portsrc


def main():
    args = sys.argv[1:]
    game = args[:1] == ["--game"]
    if game:
        args = args[1:]
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
    i.write_text(eeconst.transform(r.stdout))
    r = subprocess.run(cmd + ["-fpreprocessed", str(i), "-o", str(out)])
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
