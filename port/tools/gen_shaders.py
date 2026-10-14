"""The renderer's embedded data: the shaders (SPIR-V, the GL sources, and MSL on macOS) and the side bars' pictures,
written as C arrays to port/build/gen/gs/shaders.h and bars_png.h. Used by undefined.py and the Makefile.

Usage: port/tools/gen_shaders.py   (exit code 1 if a shader failed to compile)"""
import pathlib, shutil, subprocess, sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from toolchain import ROOT, MAC


def write_if_changed(path, text):
    """Leaves an unchanged header alone, so that make does not rebuild the renderer for nothing."""
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def generate():
    """Writes the two headers; returns the gen/gs folder and whether every shader compiled."""
    ok = True
    gen = ROOT / "port/build/gen/gs"
    gen.mkdir(parents=True, exist_ok=True)
    arrays = []
    # gsnz.frag: gs.frag without its depth output (NO_FRAG_DEPTH), for the draws that do not need it
    nz = gen / "gsnz.frag"
    nz_text = (ROOT / "port/src/gs/shaders/gs.frag").read_text().replace("\n", "\n#define NO_FRAG_DEPTH\n", 1)
    if not nz.exists() or nz.read_text() != nz_text:
        nz.write_text(nz_text)
    shader_sources = sorted((ROOT / "port/src/gs/shaders").glob("*.*")) + [nz]
    for src in shader_sources:  # gs.vert -> kGsVertSpv, outline.frag -> kOutlineFragSpv
        stage = src.suffix[1:]
        name = "k" + src.stem.capitalize() + stage.capitalize() + "Spv"
        spv = gen / (src.name + ".spv")
        if shutil.which("glslc") is None and spv.exists() and spv.stat().st_mtime >= src.stat().st_mtime:
            pass  # no shader compiler here (the release container): the compiled shader that was put in place is current
        else:
            if shutil.which("glslc") is not None:
                r = subprocess.run(["glslc", f"-fshader-stage={stage}", str(src), "-o", str(spv)], capture_output=True, text=True)
            else:  # the reference compiler of the same language (package glslang-tools)
                r = subprocess.run(["glslangValidator", "-V", "-S", stage, str(src), "-o", str(spv)], capture_output=True, text=True)
            if r.returncode:
                print("FAILED shader", src.name, r.stderr[:300])
                ok = False
                continue
        arrays.append(f"static const unsigned char {name}[] = {{" + ",".join(str(b) for b in spv.read_bytes()) + "};\n")
        if MAC:  # SDL_GPU's Metal back end takes MSL: the same shader translated (bindings as numbered: SDL's Metal slots)
            msl = gen / (src.name + ".msl")
            r = subprocess.run(["spirv-cross", "--msl", "--msl-version", "20100", "--msl-decoration-binding", str(spv), "--output", str(msl)],
                               capture_output=True, text=True)
            if r.returncode:
                print("FAILED shader (MSL)", src.name, r.stderr[:300])
                ok = False
                continue
            arrays.append(f"static const unsigned char {name[:-3]}Msl[] = {{" + ",".join(str(b) for b in msl.read_bytes() + b"\0") + "};\n")
    # The GL back end (gs_gl.c) compiles the GLSL at run time (it translates Vulkan GLSL to GL 3.3 the way
    # port/tools/glprobe.c proved), so the sources are embedded here too, NUL-terminated.
    for src in shader_sources:
        if src.suffix not in (".vert", ".frag"):
            continue
        stage = src.suffix[1:]
        name = "k" + src.stem.capitalize() + stage.capitalize() + "Glsl"
        data = src.read_bytes() + b"\0"
        arrays.append(f"static const unsigned char {name}[] = {{" + ",".join(str(b) for b in data) + "};\n")
    # the side bars' two pictures (port/assets/bars), as bytes in the program: nothing to ship beside it
    bars = "".join(f"static const unsigned char kBars{n.capitalize()}Png[] = {{" + ",".join(str(b) for b in (ROOT / "port/assets/bars" / (n + ".png")).read_bytes()) + "};\n"
                   for n in ("trim", "fill"))
    write_if_changed(gen / "bars_png.h", "/* generated from port/assets/bars by port/tools/gen_shaders.py */\n" + bars)
    write_if_changed(gen / "shaders.h", "/* generated from port/src/gs/shaders by port/tools/gen_shaders.py */\n" + "".join(arrays))
    return gen, ok


if __name__ == "__main__":
    sys.exit(0 if generate()[1] else 1)
