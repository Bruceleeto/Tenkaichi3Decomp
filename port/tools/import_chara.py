#!/usr/bin/env python3
"""import_chara.py - make a character package (docs/port/character_packages.md) from a character of a game data folder.

    port/tools/import_chara.py <data folder> <character id 0..160> <output folder> [--name "Name In The Menu"] [--replace]
    port/tools/import_chara.py <data folder> <id> <folder of folders> --with-forms [--with-fusions] [--prefix P] [--replace]

Without --replace the folder ADDS a character (its character.txt says `base=<id>`). With --replace it REPLACES the
disc's character of that id (`replaces=<id>`): put into `characters/` as it is, the game plays exactly as before,
and every file in it can then be edited or taken out (a file the folder does not have stays the disc's). That is
the start of a mod of an existing character: export, edit, drop back in.

With --with-forms the character's transformations are imported too, and theirs: a folder named as the disc
names the character, its forms in folders inside it named by their labels ("Super Saiyan"), with link lines in
each character.txt that name the others (".." is the character from inside a form's folder): the set works as it does on its
disc. A name the disc does not give is the prefix ("C" unless given) and the id. --with-fusions follows the fusions as well: their results, the
partners, and the forms of all of those (that can be many folders; the port takes 57).

    port/tools/import_chara.py <data folder or .iso> <folder of folders> --all

    port/tools/import_chara.py <data folder or .iso> <folder> --stages [--against <original data folder or .iso>]

--stages copies the disc's stages (the single-screen model, file 0x171 + stage) as `<Name>.unk` files, which is
what the `stages` folder next to the game takes (docs/port/adding_stages.md; 26 at once). The names are the places'
names in the original game: a name on a disc is a picture, so what a changed stage is called there cannot be read;
rename the files; `stage names.png`, written beside them, shows the disc's own names in the stages' order. With --against only the stages that differ from that disc's are copied.

    port/tools/import_chara.py <folder of loose files> <output folder> --loose [--name N] [--base ID] [--data <game>]

--loose makes a character folder from files taken out of a disc one by one, as mods are often passed on: any
extension (.unk, .pak, .bin), the archive's number of the file somewhere in the name ("model(2918).unk"). The
number says what a file is (a costume, the animations, the effects, a voice bank, the portrait) and the contents are
looked at as well. Without --base the base character is the original whose model is most like this one (that needs
--data, the game's data folder or disc image) or else the character whose place the number names.

<data folder> may also be a disc image (.iso): its two archives are read in place, nothing is unpacked. --all makes
a folder for every character of the disc, forms inside (more than the port takes at once: a shelf to pick from).

<data folder> is an unpacked disc in the layout the port uses (pzs3us1/NNNNN.bin, pzs3us2/NNNNN.bin): the player's
own game, or another disc of the same layout whose characters were replaced. Everything that belongs to the one
character is copied as it is, under the names the port looks for in a folder of `characters/`:

    <n>_1p.pak .. <n>_4p.pak, <n>_1p_dmg.pak ..   the models (file 0x590 + id * 10 + 0..7)
    <n>_anm.pak, <n>_eff.pak                      animations and effects (+ 8, + 9)
    <n>_voice_jp.pak, <n>_voice_us.pak            the voice banks (0xBDA + id Japanese, 0xC7B + id English)
    voice_jp/NNN.adx, voice_us/NNN.adx            the spoken lines (0x8D4E + id * 100 + line, 0xCC32 + ...)
    portrait.bin                                  the large picture of the menus (0x2F9 + id)
    member_1p.bin, member_2p.bin                  the team mate's small picture in the fight (8 + id * 2 + side)
    chip.bin                                      the select's chip (menu archive, file 0x1C1)
    character.txt                                 the manifest: format=1, name, form (the label under the name),
                                                  and base=<id> (the tables of the game the folder is put into
                                                  are asked for this character: health, cost, the lines before a
                                                  fight) or, with --replace, replaces=<id>; the links to the
                                                  other folders of a set (left= up= right= down= fusion1.. partner1..)

(Until 2026-10-13 the two voice names were the other way round, and the texts were in files of their own, name.txt,
form.txt, base.txt, forms.txt, which the game still reads. A folder made then has its two languages exchanged now:
make it again, or exchange the names.)

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


class IsoFile:
    """One file of an archive inside a disc image, with what this script asks of a path."""

    def __init__(self, iso, at, size):
        self.iso, self.at, self.size = iso, at, size

    def exists(self):
        return self.size > 0

    def stat(self):
        return type("Stat", (), {"st_size": self.size})

    def read_bytes(self):
        self.iso.seek(self.at)
        return self.iso.read(self.size)


class IsoData:
    """A disc image: DATA/PZS3US1.AFS and DATA/PZS3US2.AFS found through the ISO 9660 directory, their tables read
    ("AFS", count, then offset and size of each file)."""

    def __init__(self, path):
        self.iso = open(path, "rb")
        self.arc = {}
        self.iso.seek(16 * 2048 + 156)
        root = self.iso.read(34)
        for name, lba, size in self.entries(*struct.unpack_from("<I4xI", root, 2)):
            if name.upper() == "DATA":
                for inner, at, _ in self.entries(lba, size):
                    key = inner.upper().split(";")[0]
                    if key in ("PZS3US1.AFS", "PZS3US2.AFS"):
                        self.iso.seek(at * 2048)
                        magic, count = struct.unpack("<4sI", self.iso.read(8))
                        if magic[:3] != b"AFS":
                            sys.exit(f"{key} of this disc is not an archive of the usual kind; unpack the disc another way")
                        self.arc[key[:7].lower()] = (at * 2048, struct.unpack(f"<{count * 2}I", self.iso.read(count * 8)))
        if len(self.arc) != 2:
            sys.exit("no DATA/PZS3US1.AFS and DATA/PZS3US2.AFS on this disc")

    def entries(self, lba, size):
        self.iso.seek(lba * 2048)
        d, i = self.iso.read(size), 0
        while i < len(d):
            n = d[i]
            if n == 0:
                i = (i // 2048 + 1) * 2048
                continue
            name = d[i + 33:i + 33 + d[i + 32]]
            if name not in (b"\0", b"\1"):
                yield (name.decode("ascii", "replace"),) + struct.unpack_from("<I4xI", d, i + 2)
            i += n

    def file(self, arc, index):
        base, tab = self.arc[arc]
        if index * 2 + 1 >= len(tab):
            return IsoFile(self.iso, 0, 0)
        return IsoFile(self.iso, base + tab[index * 2], tab[index * 2 + 1])


def game_file(data, file_id):
    """The file of a file id: ids below 0xD48 are the first archive's (named id - 1), the rest the second's."""
    if isinstance(data, IsoData):
        return data.file("pzs3us1", file_id - 1) if file_id < 0xD48 else data.file("pzs3us2", file_id - 0xD48)
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


def write_manifest(out, pairs):
    """character.txt: the lines `key=value` in the order given."""
    (out / "character.txt").write_text("".join(f"{k}={v}\n" for k, v in pairs), encoding="utf-8")


def read_manifest(out):
    lines = (out / "character.txt").read_text(encoding="utf-8").split("\n")
    return [tuple(line.split("=", 1)) for line in lines if "=" in line and not line.startswith("#")]


def import_one(data, c, out, name=None, quiet=False, replace=False):
    out.mkdir(parents=True, exist_ok=True)
    n = out.name
    pairs = [("format", "1")]

    def copy(file_id, target, least=0):
        src = game_file(data, file_id)
        if not src.exists() or src.stat().st_size <= least:
            return 0
        (out / target).parent.mkdir(parents=True, exist_ok=True)
        body = src.read_bytes()
        if not body.strip(b"\0"):
            return 0
        (out / target).write_bytes(body)
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
    copy(0xBDA + c, f"{n}_voice_jp.pak")
    copy(0xC7B + c, f"{n}_voice_us.pak")
    spoken = [sum(1 for line in range(100) if copy(first + c * 100 + line, f"voice_{tag}/{line:03d}.adx"))
              for tag, first in (("jp", 0x8D4E), ("us", 0xCC32))]
    portrait = game_file(data, 0x2F9 + c)
    if portrait.exists():
        (out / "portrait.bin").write_bytes(portrait.read_bytes().rstrip(b"\0"))
    for side in (0, 1):
        copy(8 + c * 2 + side, f"member_{side + 1}p.bin")
    shown = name or n

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
        shown = name or own
        pairs.append(("name", shown))
        forms = section(res, ro, 44)  # the label under the name ("Super Saiyan"); many characters have none
        line = section(forms, table(forms), c + 1)
        label = (line[2:] if line[:2] == b"\xff\xfe" else line).decode("utf-16-le", "replace").split("\0")[0].strip()
        pairs.append(("form", label))
        say(f"name: {name or own!r}; chip saved")
    except Exception as err:  # (another edition's menu archive: the folder works without, with its base's chip)
        print("name and chip not read:", err)
        pairs = [("format", "1")] + ([("name", name)] if name else [])
    pairs.append(("replaces" if replace else "base", str(c)))
    write_manifest(out, pairs)

    # the forms: the character's table (entry 18 of the model's pack) names them by id; an id means the character
    # of that number in the game the folder is put into, unless the links of character.txt say otherwise
    model = (out / f"{n}_1p.pak").read_bytes()
    par = section(model, table(model), 18)
    trans = [x for x in par[0x98:0x9C] if x != 0xFF]
    fusion = [x for x in par[0xB4:0xB7] if x != 0xFF]
    say(f"{costumes} costumes, spoken lines {spoken[0]} (jp) / {spoken[1]} (us), "
        + (f"replaces character {c}" if replace else f"base character {c}"))
    if (trans or fusion) and not quiet and not replace:
        print(f"its table names other characters by id: transformations {trans}, fusions {fusion}. Put into a game, "
              "those ids are that game's characters; import them too and name them in character.txt "
              "(left= up= right= down= fusion1.. partner1..) to have this disc's.")
    return shown


def menu_names(data):
    """The names and the form labels of the character select (sections 43 and 44 of its pack), by character id."""
    try:
        arc = game_file(data, 0x1C1).read_bytes()
        res = bpe(section(arc, table(arc), 2))
        ro = table(res)
        out = []
        for sec in (43, 44):
            b = section(res, ro, sec)
            t = table(b)
            lines = [section(b, t, i) for i in range(1, len(t))]
            out.append([(l[2:] if l[:2] == b"\xff\xfe" else l).decode("utf-16-le", "replace").split("\0")[0].strip()
                        for l in lines])
        return out
    except Exception:  # (another edition's menu archive: the folders are named by number)
        return [], []


def clean(text):
    """A name as a folder's: without what a file system or the port's own names ("A/B", "..", "disc:") would trip on."""
    text = "".join(ch if ch.isalnum() or ch in " -_'()+!&" else " " for ch in text)
    text = " ".join(text.split()).strip(" .")
    while len(text.encode()) > 56:
        text = text[:-1].rstrip()
    return text


def import_set(data, firsts, root, prefix, fusions, shelf=False, replace=False):
    """The character and everyone its table reaches: a folder for each character, its forms in folders inside it,
    named as the disc names them, with the links in each character.txt naming the folders. (With `replace` every
    folder replaces its own disc character and no links are written: the ids in the tables are the right ones.)"""
    todo, tables, first = list(firsts), {}, firsts[0]
    while todo:
        c = todo.pop(0)
        if c in tables:
            continue
        par = params(data, c)
        if par is None:
            if not shelf:
                print(f"character {c} is named by a table but has no model on this disc; left out")
            continue
        tables[c] = par
        todo += [x for x in par[0x98:0x9C] if x != 0xFF]
        if fusions:
            for i in range(3):
                if par[0xB4 + i] != 0xFF:
                    todo.append(par[0xB4 + i])
                    todo += [x for x in par[0xBA + i * 4:0xBE + i * 4] if x != 0xFF]
    if len(tables) > 57 and not shelf:
        sys.exit(f"{len(tables)} characters hang together here; the port takes 57. Without --with-fusions there are fewer.")

    # families: the characters that transformations join. The one no other turns into (left, up, right) is the
    # character; the rest are its forms.
    family = {c: c for c in tables}

    def top(c):
        while family[c] != c:
            c = family[c]
        return c
    for c, par in tables.items():
        for x in par[0x98:0x9C]:
            if x in tables:
                family[top(x)] = top(c)
    groups = {}
    for c in tables:
        groups.setdefault(top(c), []).append(c)
    names, labels = menu_names(data)
    place, used_top = {}, set()
    for members in groups.values():
        reached = {x for c in members for x in tables[c][0x98:0x9B] if x in tables and x != c}
        heads = sorted(c for c in members if c not in reached) or sorted(members)
        head = first if first in heads else heads[0]
        name = clean(names[head]) if head < len(names) else ""
        if not name or name.lower() in used_top:
            name = f"{name or prefix} {head}".strip()
        used_top.add(name.lower())
        place[head] = (name, None)
        used = set()
        for c in sorted(members):
            if c == head:
                continue
            leaf = clean(labels[c]) if c < len(labels) else ""
            if not leaf and c < len(names) and clean(names[c]).lower() != name.lower():
                leaf = clean(names[c])  # (a form without a label that has a name of its own: "Super Vegito")
            if not leaf or leaf.lower() in used or leaf.lower() in ("voice_us", "voice_jp"):
                leaf = f"{leaf or prefix} {c}".strip()
            used.add(leaf.lower())
            place[c] = (name, leaf)

    def ref(src, dst):  # how src's links name dst
        (st, _), (dt, dl) = place[src], place[dst]
        if st == dt:
            return dl if dl is not None else ".."
        return dt if dl is None else f"{dt}/{dl}"

    for c in sorted(tables, key=lambda x: (place[x][0].lower(), place[x][1] is not None, x)):
        par = tables[c]
        t, leaf = place[c]
        folder = root / t if leaf is None else root / t / leaf
        shown = import_one(data, c, folder, quiet=True, replace=replace)
        lines = [f"{key}={ref(c, par[0x98 + i])}" for i, key in enumerate(("left", "up", "right", "down"))
                 if par[0x98 + i] in tables and par[0x98 + i] != c and not replace]
        for i in range(0 if replace else 3):
            # only when the fusions were asked for (or the whole disc is made): else the ids stay the characters of
            # the game the folder is put into, and no folder is named that may not be there
            if (fusions or shelf) and par[0xB4 + i] in tables:
                lines.append(f"fusion{i + 1}={ref(c, par[0xB4 + i])}")
                who = list(dict.fromkeys(ref(c, x) for x in par[0xBA + i * 4:0xBE + i * 4] if x in tables))
                if who:
                    lines.append(f"partner{i + 1}=" + ",".join(who))
        if lines:
            write_manifest(folder, read_manifest(folder) + [tuple(line.split("=", 1)) for line in lines])
        print(f"{t if leaf is None else '  ' + leaf}  [{c}: {shown}]" + (f"  {', '.join(lines)}" if lines else ""))
    print(f"{len(tables)} characters in {len(groups)} folder{'s' if len(groups) != 1 else ''} of {root}")


def pack_count(body):
    """The number of entries, if `body` is a pack of the game's kind (a count and that many offsets), else 0."""
    if len(body) < 8:
        return 0
    (n,) = struct.unpack_from("<I", body, 0)
    if not 0 < n < 600 or len(body) < 4 * (n + 2):
        return 0
    first = struct.unpack_from("<I", body, 4)[0] & ~3
    return n if 4 * (n + 2) <= first <= len(body) else 0


def import_loose(src, out, name, base, data):
    import re
    found = {}  # kind -> (path, what the number said)
    notes = []
    for path in sorted(p for p in src.rglob("*") if p.is_file()):
        body = path.read_bytes()
        count = pack_count(body)
        numbers = re.findall(r"\d{2,5}", path.stem)
        index = int(numbers[-1]) if numbers else None
        kind = who = None
        low = path.stem.lower()  # a file already named as a pack's are ("x_1p_dmg", "x_anm", "x_voice_us")
        named = re.search(r"_([1-4])p(_dmg)?$", low)
        if named:
            kind = ("dmg" if named.group(2) else "model") + str(int(named.group(1)) - 1)
        else:
            kind = next((k for k in ("anm", "eff", "voice_us", "voice_jp") if low.endswith("_" + k)), None)
        if kind is not None:
            index = None
        if index is not None:
            fid = index + 1  # (the tools number from 0: the file id is one more)
            if 0x590 <= fid < 0x590 + 1610:
                who, k = divmod(fid - 0x590, 10)
                kind = ("anm" if k == 8 else "eff" if k == 9 else f"dmg{k - 4}" if k >= 4 else f"model{k}")
            elif 0xBDA <= fid < 0xBDA + 161:
                who, kind = fid - 0xBDA, "voice_jp"
            elif 0xC7B <= fid < 0xC7B + 161:
                who, kind = fid - 0xC7B, "voice_us"
            elif 0x2F9 <= fid < 0x2F9 + 161:
                who, kind = fid - 0x2F9, "portrait"
        # the contents decide where the number is missing or says something else
        by_count = {252: "model", 440: "anm", 6: "eff", 3: "voice_us"}.get(count)
        if kind is None or (by_count and not kind.startswith(by_count[:3]) and kind != "portrait"
                            and not (by_count == "model" and kind.startswith("dmg"))
                            and not (by_count == "voice_us" and kind == "voice_jp")):
            if by_count is None:
                notes.append(f"{path.name}: not recognised, left out")
                continue
            kind = by_count + ("0" if by_count == "model" else "")
            who = None
        while kind in found and kind[:-1] in ("model", "dmg") and int(kind[-1]) < 3:
            kind = kind[:-1] + str(int(kind[-1]) + 1)  # (a second model of the same place: the next costume)
        if kind in found:
            notes.append(f"{path.name}: a second file for {kind}, left out")
            continue
        found[kind] = (path, who)
    models = sorted(k for k in found if k.startswith("model"))
    damaged = sorted(k for k in found if k.startswith("dmg"))
    if not models and damaged:  # (only damaged models were passed on: they are the costumes)
        models, damaged = damaged, []
    if not models:
        sys.exit("no character model among these files (a pack of 252 entries): " + "; ".join(notes))
    out.mkdir(parents=True, exist_ok=True)
    n = out.name
    plan = [(k, f"{n}_{i + 1}p.pak") for i, k in enumerate(models)]
    plan += [(k, f"{n}_{models.index('model' + k[-1]) + 1}p_dmg.pak") for k in damaged if "model" + k[-1] in models]
    plan += [(k, f"{n}_{k}.pak") for k in ("anm", "eff", "voice_us", "voice_jp") if k in found]
    if "portrait" in found:
        plan.append(("portrait", "portrait.bin"))
    for kind, target in plan:
        body = found[kind][0].read_bytes()
        (out / target).write_bytes(body.rstrip(b"\0") if kind == "portrait" else body)
        print(f"{target:28s} <- {found[kind][0].name}")
    for note in notes:
        print(note)
    model = found[models[0]][0].read_bytes()
    why = "given"
    if base is None and data is not None:  # the original whose model shares the most entries with this one
        t = table(model)
        mine = [section(model, t, i) for i in range(1, len(t))]
        best = (0, None)
        for c in range(161):
            src_file = game_file(data, 0x590 + c * 10)
            if not src_file.exists():
                continue
            other = src_file.read_bytes()
            if pack_count(other) != len(mine):
                continue
            ot = table(other)
            same = sum(1 for i, e in enumerate(mine) if len(e) and e == section(other, ot, i + 1))
            if same > best[0]:
                best = (same, c)
        base, why = best[1], f"the original whose model is most like it ({best[0]} of {len(mine)} entries the same)"
    if base is None:
        base, why = found[models[0]][1], "the character whose place the file's number names"
    if base is None:
        base, why = 0, "nothing said which: Goku (Early). Give --base or --data"
    write_manifest(out, [("format", "1"), ("name", name or n), ("base", str(base))])
    print(f"base character {base}: {why}")
    par = section(model, table(model), 18)
    forms = [x for x in par[0x98:0x9C] if x != 0xFF] + [x for x in par[0xB4:0xB7] if x != 0xFF]
    if forms:
        print(f"its table names other characters by id {forms} (transformations, fusions): see the links of character.txt in docs/port/character_packages.md")
    missing = [w for w, k in (("voice bank", "voice_jp"), ("animations", "anm"), ("effects", "eff")) if k not in found]
    print("not among the files, so the base character's: " + ", ".join(missing + ["chip", "spoken lines"] + ([] if "portrait" in found else ["portrait"])))


STAGES = (  # the original's stages by id (port/src/gs/namefont.c, kStageNames)
    "Wasteland - Noon", "Rocky Area - Noon", "Planet Namek", "Dying Namek", "World Tournament Stage - Noon",
    "Kami's Lookout", "Cell Games Arena - Evening", "Supreme Kai's World", "Hyperbolic Time Chamber",
    "City Ruins - Noon", "Mountain Road - Noon", "Islands", "Kame House", "Planet - Night", "Glacier", "Ruined Earth",
    "Outer Space", "Penguin Village", "Hell", "Desert - Noon", "King's Castle", "Muscle Tower", "Mount Paozu",
    "Wasteland - Evening", "Wasteland - Night", "Rocky Area - Evening", "Rocky Area - Night",
    "World Tournament Stage - Evening", "Cell Game - Noon", "City Ruins - Evening", "City Ruins - Night",
    "Desert - Evening", "Desert - Night", "Mountain Road - Evening", "Planet - Evening")
STAGE_BUF = 0x6CB800  # BTL_STAGE_BUF_SIZE: the buffer a stage's model is read into


def source(path):
    return IsoData(path) if pathlib.Path(path).is_file() else pathlib.Path(path)


# the PS2's memory layout of a picture (as port/src/gs/namefont.c, sheet_decode)
_B32 = [[0, 1, 4, 5, 16, 17, 20, 21], [2, 3, 6, 7, 18, 19, 22, 23], [8, 9, 12, 13, 24, 25, 28, 29], [10, 11, 14, 15, 26, 27, 30, 31]]
_C32 = [[0, 1, 4, 5, 8, 9, 12, 13], [2, 3, 6, 7, 10, 11, 14, 15], [16, 17, 20, 21, 24, 25, 28, 29], [18, 19, 22, 23, 26, 27, 30, 31], [32, 33, 36, 37, 40, 41, 44, 45], [34, 35, 38, 39, 42, 43, 46, 47], [48, 49, 52, 53, 56, 57, 60, 61], [50, 51, 54, 55, 58, 59, 62, 63]]
_C8 = [[0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54], [8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62], [33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23], [41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31]]


def name_sheet(sec, n):
    """Texture n of the select's stage-name file (512 x 256, 8 bits with a colour table) as rows of RGBA."""
    ent = struct.unpack_from("<I", sec, 4)[0] * 4 + n * 0x40
    pix = struct.unpack_from("<I", sec, ent)[0] * 4 + 0x60
    clut = struct.unpack_from("<I", sec, ent + 4)[0] * 4 + 0x60
    if struct.unpack_from("<II", sec, ent + 8) != (0x60 + 512 * 256 + 0x20, 0x60 + 0x400 + 0x20):
        raise ValueError("not the sheets of the usual kind")
    mem = bytearray(512 * 256)
    for y in range(128):
        for x in range(256):
            a = (((y & ~0x1F) * 4 + ((x >> 1) & ~0x1F) + _B32[(y >> 3) & 3][(x >> 3) & 7]) << 6) + _C32[y & 7][x & 7]
            mem[a * 4:a * 4 + 4] = sec[pix + (y * 256 + x) * 4:pix + (y * 256 + x) * 4 + 4]
    rows = []
    for y in range(256):
        row = bytearray()
        col = (y & 15) >> 2
        for x in range(512):
            sx = ((x & 15) ^ 4) if col & 1 else (x & 15)
            i = mem[((((y >> 1) & ~0x1F) * 4 + ((x >> 2) & ~0x1F) + _B32[(y >> 4) & 3][(x >> 4) & 7]) << 8)
                    + _C8[y & 3][sx] + col * 64]
            i = (i & 0xE7) | ((i & 8) << 1) | ((i & 0x10) >> 1)
            c = sec[clut + i * 4:clut + i * 4 + 4]
            row += bytes((c[0], c[1], c[2], min(255, c[3] * 2)))
        rows.append(bytes(row))
    return rows


def stage_names_picture(data, path):
    """The disc's 36 stage-name pictures, numbered by stage, in one PNG: what its stages are called."""
    import zlib
    arc = game_file(data, 0x1C1).read_bytes()
    res = bpe(section(arc, table(arc), 2))
    sec = section(res, table(res), 6)
    rows = []
    for n in range(9):
        rows += name_sheet(sec, n)
    # over a dark ground, as RGB
    raw = bytearray()
    for row in rows:
        raw.append(0)
        for x in range(512):
            r, g, b, a = row[x * 4:x * 4 + 4]
            raw += bytes(((r * a + 30 * (255 - a)) // 255, (g * a + 30 * (255 - a)) // 255, (b * a + 30 * (255 - a)) // 255))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 512, len(rows), 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b""))


def import_stages(data, out, against):
    out.mkdir(parents=True, exist_ok=True)
    made = same = 0
    try:  # the small pictures of the select's reel: section 46 of its pack, entry stage + 1
        arc = game_file(data, 0x1C1).read_bytes()
        res = bpe(section(arc, table(arc), 2))
        icons = section(res, table(res), 46)
        icon_at = table(icons)
    except Exception:
        icons = None
    for stage, name in enumerate(STAGES):
        src = game_file(data, 0x171 + stage)
        if not src.exists():
            continue
        body = src.read_bytes()
        if not body.strip(b"\0"):
            continue
        if against is not None:
            other = game_file(against, 0x171 + stage)
            if other.exists() and other.read_bytes().rstrip(b"\0") == body.rstrip(b"\0"):
                same += 1
                continue
        (out / f"{name}.unk").write_bytes(body)
        thumb = game_file(data, 0x39D + stage)  # the select's picture of the stage
        if thumb.exists():
            (out / f"{name}.thumb").write_bytes(thumb.read_bytes())
        if icons is not None:
            (out / f"{name}.icon").write_bytes(section(icons, icon_at, stage + 1))
        made += 1
        note = "  LARGER than the game's stage buffer: it would be cut off" if len(body.rstrip(b"\0")) > STAGE_BUF else ""
        print(f"{stage:2d}  {name}.unk  {len(body) / 1048576:.1f} MB{note}")
    print(f"{made} stages in {out}" + (f"; {same} are the same as the other disc's and were left" if against is not None else ""))
    try:  # (the names as the disc shows them: stage n is the n-th picture from the top, 0 first, 64 pixels each)
        stage_names_picture(data, out / "stage names.png")
        print("`stage names.png`: this disc's names of its stages, in the order of the numbers above (0 at the top).")
    except Exception as err:
        print("the disc's stage names could not be drawn:", err)
    print("The game takes 26 added stages at once: copy the files wanted into `stages` and rename them as you like.")


def main():
    flags = {f for f in ("--with-forms", "--with-fusions", "--all", "--stages", "--loose", "--replace") if f in sys.argv}
    args = [a for a in sys.argv[1:] if a not in flags]
    opt = {}
    for key in ("--name", "--prefix", "--against", "--base", "--data"):
        if key in args:
            at = args.index(key)
            opt[key] = args[at + 1]
            del args[at:at + 2]
    if len(args) != (2 if flags & {"--all", "--stages", "--loose"} else 3):
        sys.exit(__doc__)
    if "--loose" in flags:
        import_loose(pathlib.Path(args[0]), pathlib.Path(args[1]), opt.get("--name"),
                     int(opt["--base"], 0) if "--base" in opt else None, source(opt["--data"]) if "--data" in opt else None)
        return
    data = source(args[0])
    if "--stages" in flags:
        import_stages(data, pathlib.Path(args[1]), source(opt["--against"]) if "--against" in opt else None)
        return
    if "--all" in flags:
        import_set(data, list(range(161)), pathlib.Path(args[1]), opt.get("--prefix", "C"), False, shelf=True)
        print("The port takes 57 characters at once (forms count): copy the folders wanted into `characters`.")
        return
    c, out = int(args[1], 0), pathlib.Path(args[2])
    if not 0 <= c <= 160:
        sys.exit("the character id is 0..160")
    replace = "--replace" in flags
    if flags - {"--replace"}:
        import_set(data, [c], out, opt.get("--prefix", "C"), "--with-fusions" in flags, replace=replace)
    else:
        import_one(data, c, out, opt.get("--name"), replace=replace)


if __name__ == "__main__":
    main()
