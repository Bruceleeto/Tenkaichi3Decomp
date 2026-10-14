#!/usr/bin/env python3
"""The Makefile's setup and extract steps, done by install.py's own steps (same checks, same checksums).

    port/tools/mk.py setup              the requirements of the 32-bit gcc build, and the tool environment (.venv)
    port/tools/mk.py extract [ISO]      the disc's programs (disc/), the game data (gamedata/), the split (asm/)
                                        ISO: the disc image; without it, the one .iso next to the repository"""
import argparse, pathlib, sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
import install


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("setup", "extract"):
        sys.exit(__doc__)
    what = sys.argv[1]
    args = argparse.Namespace(bits32=True, iso=sys.argv[2] if len(sys.argv) > 2 and sys.argv[2] else None, force=False)
    install.LOG.write_text(f"make {what}\n")
    try:
        if what == "setup":
            install.Step.total = 2
            install.step_requirements(args)
            s = install.Step("Tool environment")
            py = install.venv_python()
            if not py.exists() and install.run([sys.executable, "-m", "venv", "--system-site-packages", install.VENV])[0]:
                s.fail("Could not create the Python environment .venv (see install.log).")
            if install.run([py, "-c", "import splat"])[0] and \
                    install.run([py, "-m", "pip", "install", "--quiet", "--disable-pip-version-check", install.SPLAT])[0]:
                s.fail(f"Could not install {install.SPLAT} (see install.log). This step needs an internet connection once.")
            s.ok(".venv")
        else:
            install.Step.total = 3
            iso = install.step_disc(args)
            install.step_gamedata(args, iso)
            install.step_split(args)
    except install.Failed as e:
        print(f"\n  {install.BAD('Stopped.')} {e}\n  {install.DIM('Details: install.log.')}\n")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
