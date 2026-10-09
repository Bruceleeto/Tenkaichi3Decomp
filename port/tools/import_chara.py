#!/usr/bin/env python3
"""import_chara.py - make a character folder (docs/port/adding_characters.md) from a character of a game data folder.

    port/tools/import_chara.py <data folder> <character id 0..160> <output folder> [--name "Name In The Menu"]
    port/tools/import_chara.py <data folder> <id> <folder of folders> --with-forms [--with-fusions] [--prefix P]

With --with-forms the character's transformations are imported too, and theirs, each into its own folder
(<prefix><id>, the prefix "C" unless given) inside the output folder, with a forms.txt in each that names the
others: the set works as it does on its disc. --with-fusions follows the fusions as well: their results, the
partners, and the forms of all of those (that can be many folders; the port takes 57).

<data folder> is an unpacked disc in the layout the port uses (pzs3us1/NNNNN.bin, pzs3us2/NNNNN.bin): the player's
own game, or another disc of the same layout whose characters were replaced. Everything that belongs to the one
character is copied as it is, under the names the port looks for in a folder of `characters/`:

    <n>_1p.pak .. <n>_4p.pak, <n>_1p_dmg.pak ..   the models (file 0x590 + id * 10 + 0..7)
    <n>_anm.pak, <n>_eff.pak                      animations and effects (+ 8, + 9)
    <n>_voice_us.pak, <n>_voice_jp.pak            the voice banks (0xBDA + id, 0xC7B + id)
    voice_us/NNN.adx, voice_jp/NNN.adx            the spoken lines (0x8D4E + id * 100 + line, 0xCC32 + ...)
    portrait.bin                                  the large picture of the menus (0x2F9 + id)
    member_1p.bin, member_2p.bin                  the team mate's small picture in the fight (8 + id * 2 + side)
    chip.bin, name.txt                            the select's chip and the name (menu archive, file 0x1C1)
    base.txt                                      the id: the tables of the game the folder is put into are asked
                                                  for this character (health, cost, the lines before a fight)

Nothing is converted. The folder holds game data: it is for the player's own use and is not to be passed on with
the game's own art in it.
"""
import pathlib
import struct
import sys


def bpe(src):
    """Bpe_Decode (src/sys/bpe.c): size, packed size, blocks of a pair table and literal bytes."""
    size, packed = struct.unpack_from("<ii", src, 0)
    p, end, out = 8, 8 + packed, bytearray()
    while p < end:
        left, right = list(range(256)), [0] * 256
        count, c = src[p], 0
        p += 1
        while True:
            if count > 127:
                c += count - 127
                count = 0
            if c == 256:
                break
            for _ in range(count + 1):
                left[c] = src[p]
                p += 1
                if c != left[c]:
                    right[c] = src[p]
                    p += 1
                c += 1
            if c == 256:
                break
            count = src[p]
            p += 1
        n = (src[p] << 8) + src[p + 1]
        p += 2
        stack = []
        while True:
            if stack:
                c = stack.pop()
            else:
                if n == 0:
                    break
                n -= 1
                c = src[p]
                p += 1
            if c == left[c]:
                out.append(c)
            else:
                stack.append(right[c])
                stack.append(left[c])
    return bytes(out[:size])


def table(b):
    """A pack as MPACK_AT reads it: word 0 the count, word n the offset of section n (low two bits are flags)."""
    (n,) = struct.unpack_from("<I", b, 0)
    return [struct.unpack_from("<I", b, 4 * i)[0] & ~3 for i in range(1, n + 2)]


def section(b, offs, i):
    return b[offs[i - 1]:offs[i]] if i < len(offs) else b[offs[i - 1]:]


def game_file(data, file_id):
    """The file of a file id: ids below 0xD48 are the first archive's (named id - 1), the rest the second's."""
    if file_id < 0xD48:
        return data / "pzs3us1" / f"{file_id - 1:05d}.bin"
    return data / "pzs3us2" / f"{file_id - 0xD48:05d}.bin"


def params(data, c):
    """The character's table (entry 18 of its first model's pack), or None where there is no such character."""
    src = game_file(data, 0x590 + c * 10)
    if not src.exists() or src.stat().st_size <= 4096:
        return None
    model = src.read_bytes()
    return section(model, table(model), 18)


def import_one(data, c, out, name=None, quiet=False):
    out.mkdir(parents=True, exist_ok=True)
    n = out.name

    def copy(file_id, target, least=0):
        src = game_file(data, file_id)
        if not src.exists() or src.stat().st_size <= least:
            return 0
        (out / target).parent.mkdir(parents=True, exist_ok=True)
        (out / target).write_bytes(src.read_bytes())
        return src.stat().st_size

    costumes = 0
    for k in range(4):  # (a costume the character does not have is a file of a sector or less)
        size = copy(0x590 + c * 10 + k, f"{n}_{k + 1}p.pak", 4096)
        if size == 0:
            break
        costumes += 1
        copy(0x590 + c * 10 + 4 + k, f"{n}_{k + 1}p_dmg.pak", 4096)
        if size > 0x180000:
            print(f"costume {k + 1}: {size} bytes, more than the 1.5 MB the port takes; the character would be left out")
    if costumes == 0:
        sys.exit(f"no model of character {c} in {data}")
    say = (lambda *a: None) if quiet else print
    copy(0x590 + c * 10 + 8, f"{n}_anm.pak")
    copy(0x590 + c * 10 + 9, f"{n}_eff.pak")
    copy(0xBDA + c, f"{n}_voice_us.pak")
    copy(0xC7B + c, f"{n}_voice_jp.pak")
    spoken = [sum(1 for line in range(100) if copy(first + c * 100 + line, f"voice_{tag}/{line:03d}.adx"))
              for tag, first in (("us", 0x8D4E), ("jp", 0xCC32))]
    portrait = game_file(data, 0x2F9 + c)
    if portrait.exists():
        (out / "portrait.bin").write_bytes(portrait.read_bytes().rstrip(b"\0"))
    for side in (0, 1):
        copy(8 + c * 2 + side, f"member_{side + 1}p.bin")
    (out / "base.txt").write_text(f"{c}\n")

    # the name and the chip: the menu archive's second screen is the character select's pack (BPE-packed);
    # its section 43 is the names (a pack of UTF-16 lines), 45 the chips (a pack, entry id + 1)
    try:
        arc = game_file(data, 0x1C1).read_bytes()
        res = bpe(section(arc, table(arc), 2))
        ro = table(res)
        names = section(res, ro, 43)
        line = section(names, table(names), c + 1)
        own = (line[2:] if line[:2] == b"\xff\xfe" else line).decode("utf-16-le", "replace").split("\0")[0].strip()
        chips = section(res, ro, 45)
        (out / "chip.bin").write_bytes(section(chips, table(chips), c + 1))
        (out / "name.txt").write_text((name or own) + "\n")
        say(f"name: {name or own!r}; chip saved")
    except Exception as err:  # (another edition's menu archive: the folder works without, with its base's chip)
        print("name and chip not read:", err)
        if name:
            (out / "name.txt").write_text(name + "\n")

    # the forms: the character's table (entry 18 of the model's pack) names them by id; an id means the character
    # of that number in the game the folder is put into, unless forms.txt says otherwise
    model = (out / f"{n}_1p.pak").read_bytes()
    par = section(model, table(model), 18)
    trans = [x for x in par[0x98:0x9C] if x != 0xFF]
    fusion = [x for x in par[0xB4:0xB7] if x != 0xFF]
    say(f"{costumes} costumes, spoken lines {spoken[0]} / {spoken[1]}, base character {c}")
    if (trans or fusion) and not quiet:
        print(f"its table names other characters by id: transformations {trans}, fusions {fusion}. Put into a game, "
              "those ids are that game's characters; import them too and name them in forms.txt "
              "(left= up= right= down= fusion1.. partner1..) to have this disc's.")
    return (out / "name.txt").read_text().strip() if (out / "name.txt").exists() else n


def import_set(data, first, root, prefix, fusions):
    """The character and everyone its table reaches, a folder each, with forms.txt naming the folders."""
    todo, tables = [first], {}
    while todo:
        c = todo.pop(0)
        if c in tables:
            continue
        par = params(data, c)
        if par is None:
            print(f"character {c} is named by a table but has no model on this disc; left out")
            continue
        tables[c] = par
        todo += [x for x in par[0x98:0x9C] if x != 0xFF]
        if fusions:
            for i in range(3):
                if par[0xB4 + i] != 0xFF:
                    todo.append(par[0xB4 + i])
                    todo += [x for x in par[0xBA + i * 4:0xBE + i * 4] if x != 0xFF]
    if len(tables) > 57:
        sys.exit(f"{len(tables)} characters hang together here; the port takes 57. Without --with-fusions there are fewer.")
    folder = lambda x: f"{prefix}{x}"
    for c, par in tables.items():
        shown = import_one(data, c, root / folder(c), quiet=True)
        lines = [f"{key}={folder(par[0x98 + i])}" for i, key in enumerate(("left", "up", "right", "down"))
                 if par[0x98 + i] in tables]
        for i in range(3):
            if par[0xB4 + i] in tables:  # (only with --with-fusions; else the id stays the game's own character)
                lines.append(f"fusion{i + 1}={folder(par[0xB4 + i])}")
                who = [folder(x) for x in par[0xBA + i * 4:0xBE + i * 4] if x in tables]
                if who:
                    lines.append(f"partner{i + 1}=" + ",".join(who))
        if lines:
            (root / folder(c) / "forms.txt").write_text("\n".join(lines) + "\n")
        print(f"{folder(c)}: {shown}" + (f"  ({', '.join(lines)})" if lines else ""))
    print(f"{len(tables)} folders in {root}")


def main():
    flags = {f for f in ("--with-forms", "--with-fusions") if f in sys.argv}
    args = [a for a in sys.argv[1:] if a not in flags]
    opt = {}
    for key in ("--name", "--prefix"):
        if key in args:
            at = args.index(key)
            opt[key] = args[at + 1]
            del args[at:at + 2]
    if len(args) != 3:
        sys.exit(__doc__)
    data, c, out = pathlib.Path(args[0]), int(args[1], 0), pathlib.Path(args[2])
    if not 0 <= c <= 160:
        sys.exit("the character id is 0..160")
    if flags:
        import_set(data, c, out, opt.get("--prefix", "C"), "--with-fusions" in flags)
    else:
        import_one(data, c, out, opt.get("--name"))


if __name__ == "__main__":
    main()
