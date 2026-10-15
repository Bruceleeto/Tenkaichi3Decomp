"""Every texture entry / palette the asset files hold, with the key the runtime looks it up by
(port/src/gs/gs_pvr.c nat_get / Port_PalNative), written to OUT/tex.pkl for agg_tex.py to check against tex.pak's index.
    python3 -I tex_check.py [JOBS]
Environment: BT3_GAMEDATA (default: the tree's gamedata/), BT3_TEXPAK (default: the tree's
port/build/dc/assets/tex.pak), BT3_CHECK_OUT (default: a directory in the system's temporary directory)."""
import sys, os, struct, pickle, tempfile, collections, multiprocessing as mp
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bt3walk

TREE = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '..'))
ROOT = os.environ.get('BT3_GAMEDATA', os.path.join(TREE, 'gamedata'))
PAK = os.environ.get('BT3_TEXPAK', os.path.join(TREE, 'port', 'build', 'dc', 'assets', 'tex.pak'))
OUT = os.environ.get('BT3_CHECK_OUT', os.path.join(tempfile.gettempdir(), 'bt3_tex_check'))
M64 = (1 << 64) - 1
KEY_DIRECT = 0x9E3779B97F4A7C15
KEY_T8 = 0xC2B2AE3D27D4EB4F


def KEY_SHAPE(key, t0):
    return key ^ ((((t0 >> 14) & 0xFFFFF) + 1) * 0xD6E8FEB86659FD93 & M64)


def fnv(data):
    h = 0xCBF29CE484222325
    for c in data:
        h = ((h ^ c) * 0x100000001B3) & M64
    return h


def pkt_key(p, at, size, blt, pal=False):
    """gs_pvr.c pkt_key: (ok, key, rw, rh, image data offset). pal: a palette (Port_PalNative), whose alpha of the
    first 16 colours is left out when it is 32-bit."""
    dpsm = (blt >> 8) & 63
    bits = 32 if dpsm == 0 else 24 if dpsm == 1 else 16 if dpsm in (2, 0xA) else 8 if dpsm == 0x13 else 4
    rw = rh = 0; images = 0; want = 0; img = None
    end = at + size
    q = at
    if size >= 16 and (p[q + 15] & 0x7F) == 0x50:
        q += 16
    parts = []
    while q + 16 <= end:
        tag = struct.unpack_from('<Q', p, q)[0]
        nloop = tag & 0x7FFF; flg = (tag >> 58) & 3; nreg = (tag >> 60) & 15
        q += 16
        if flg == 0 and nreg == 1 and (p[q - 8] & 15) == 0xE:
            for i in range(nloop):
                if q + 16 > end: break
                if p[q + 8] == 0x52:
                    rw = struct.unpack_from('<I', p, q)[0] & 0xFFF; rh = struct.unpack_from('<I', p, q + 4)[0] & 0xFFF
                q += 16
        elif flg == 2:
            if images == 0:
                want = rw * rh * bits // 8; img = q
            images += 1
            n = min(nloop * 16, end - q)
            take = min(n, want)
            part = bytearray(p[q:q + take])
            if pal and dpsm == 0:  # (the slot alphas: image bytes 3, 7, .. 63)
                done = rw * rh * 4 - want
                part = bytes(c for i, c in enumerate(part) if not (done + i < 64 and (done + i) & 3 == 3))
            parts.append(bytes(part)); want -= take
            q += nloop * 16
        else:
            break
    return images != 0 and rw != 0, fnv(b''.join(parts)), rw, rh, img


def load_pak():
    with open(PAK, 'rb') as f:
        head = f.read(8)
        n = struct.unpack_from('<I', head, 4)[0]
        idx = f.read(n * 20)
    a = np.frombuffer(idx, dtype=np.dtype([('key', '<u8'), ('off', '<u4'), ('w', '<u2'), ('h', '<u2'), ('bits', 'u1'), ('psm', 'u1'), ('p', 'u2')]))
    return a


def do_file(args):
    rel, full, fid = args
    with open(full, 'rb') as f:
        b = f.read()
    if len(b) < 0x30 or b[0x20:0x26] == b'(c)CRI' or (b[0] == 0x80 and b[1] == 0):
        return rel, fid, None
    out = []   # (path, entry index, kind, psm, tw, th, key(s), note)

    def emit(kind, data, path, extra):
        if kind == 'tex':
            hdr, ents = extra
            for i, e in enumerate(ents):
                pix = bt3walk.rd32(data, e) * 4; clut = bt3walk.rd32(data, e + 4) * 4
                size = bt3walk.rd32(data, e + 8); csize = bt3walk.rd32(data, e + 12)
                pblt = bt3walk.rd32(data, e + 0x18); cblt = bt3walk.rd32(data, e + 0x1C)
                t0 = struct.unpack_from('<Q', data, e + 0x30)[0]
                psm = (t0 >> 20) & 63; tw = 1 << ((t0 >> 26) & 15); th = 1 << ((t0 >> 30) & 15)
                if pix and size:
                    ok, k, rw, rh, _ = pkt_key(data, pix, size, pblt)
                    out.append((path, i, 'tex', psm, tw, th, KEY_SHAPE(k, t0) if ok else None, (pblt >> 8) & 63, rw, rh))
                    if psm in (0, 1, 2, 0xA) and (pblt >> 8) & 63 in (0, 2, 0xA) and (rw, rh) in ((16, 16), (8, 2)):
                        # (a palette kept as an entry of its own: Port_TexNative asks Port_PalNative for its pixels)
                        ok, k, rw, rh, _ = pkt_key(data, pix, size, pblt, True)
                        out.append((path, i, 'entrypal', (pblt >> 8) & 63, rw, rh, k if ok else None, (pblt >> 8) & 63, rw, rh))
                if clut and csize:
                    ok, k, rw, rh, _ = pkt_key(data, clut, csize, cblt, True)
                    out.append((path, i, 'pal', (cblt >> 8) & 63, rw, rh, k if ok else None, (cblt >> 8) & 63, rw, rh))
        elif kind == 'font':
            e = extra
            pix = bt3walk.rd32(data, e); clut = bt3walk.rd32(data, e + 4)
            size = bt3walk.rd32(data, e + 8); csize = bt3walk.rd32(data, e + 12)
            pblt = bt3walk.rd32(data, e + 0x18); cblt = bt3walk.rd32(data, e + 0x1C)
            t0 = struct.unpack_from('<Q', data, e + 0x30)[0]
            psm = (t0 >> 20) & 63; tw = 1 << ((t0 >> 26) & 15); th = 1 << ((t0 >> 30) & 15)
            ok, k, rw, rh, _ = pkt_key(data, pix, size, pblt)
            out.append((path, 0, 'fonttex', psm, tw, th, KEY_SHAPE(k, t0) if ok else None, (pblt >> 8) & 63, rw, rh))
            ok, k, rw, rh, _ = pkt_key(data, clut, csize, cblt, True)
            out.append((path, 0, 'fontpal', (cblt >> 8) & 63, rw, rh, k if ok else None, (cblt >> 8) & 63, rw, rh))
    bt3walk.walk(b, rel, 0, emit)
    return rel, fid, out


def main():
    os.makedirs(OUT, exist_ok=True)
    jobs = [j for j in bt3walk.files(ROOT) if not j[0].startswith('pzs3us2')]
    res = {}
    with mp.Pool(int(sys.argv[1]) if len(sys.argv) > 1 else 4) as pool:
        for i, (rel, fid, out) in enumerate(pool.imap_unordered(do_file, jobs, chunksize=4)):
            if out is not None:
                res[rel] = (fid, out)
            if i % 500 == 0:
                print(i, len(jobs), flush=True)
    with open(os.path.join(OUT, 'tex.pkl'), 'wb') as f:
        pickle.dump(res, f)
    print('done', len(res))


if __name__ == '__main__':
    main()
