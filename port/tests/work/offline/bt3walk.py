"""Walk the game's archive files the way port/tools/dc_tex.c does: a file is a texture file, a font, a BPE-packed
part, an offset table of parts (u32 count, count offsets), or a format of its own. Yields every node."""
import struct, os

def rd32(b, o):
    return struct.unpack_from('<I', b, o)[0]

def psm_bits(psm):
    return {0: 32, 1: 24, 2: 16, 0xA: 16, 0x13: 8, 0x1B: 8, 0x14: 4, 0x24: 4, 0x2C: 4}.get(psm, 0)

def upload_ok(p, at0, size):
    """an upload packet: [VIF DIRECT] then GIF A+D tags then IMAGE; returns list of (regs dict, image qw) or None"""
    at = at0
    end = at0 + size
    if size >= 16 and (p[at + 15] & 0x7F) == 0x50:
        at += 16
    images = 0
    regs = {}
    while at + 16 <= end:
        tag, rg = struct.unpack_from('<QQ', p, at)
        nloop = tag & 0x7FFF; flg = (tag >> 58) & 3; nreg = (tag >> 60) & 15
        at += 16
        if flg == 0 and nreg == 1 and (rg & 15) == 0xE:
            for i in range(nloop):
                if at + 16 > end: break
                d, r = struct.unpack_from('<QQ', p, at)
                regs[r & 0xFF] = d
                at += 16
        elif flg == 2:
            if regs.get(0x52, 0) & 0xFFF == 0:
                return None
            images += 1
            at += nloop * 16
        else:
            break
    return images

def bpe(src):
    if len(src) < 8: return None
    size, pack = struct.unpack_from('<II', src, 0)
    n = len(src)
    if pack > n - 8 or size > (64 << 20): return None
    out = bytearray()
    i = 8; end = 8 + pack
    left = [0] * 256; right = [0] * 256
    try:
        while i < end:
            c = 0; count = src[i]; i += 1
            for k in range(256): left[k] = k
            while True:
                if count > 127:
                    c += count - 127; count = 0
                if c == 256: break
                k = 0
                while k <= count and c < 256:
                    if i >= end: return None
                    left[c] = src[i]; i += 1
                    if c != left[c]:
                        if i >= end: return None
                        right[c] = src[i]; i += 1
                    k += 1; c += 1
                if c == 256: break
                if i >= end: return None
                count = src[i]; i += 1
            if i + 2 > end: return None
            bs = src[i] << 8 | src[i + 1]; i += 2
            stack = []
            while True:
                if stack:
                    c = stack.pop()
                else:
                    if bs == 0: break
                    bs -= 1
                    if i >= end: return None
                    c = src[i]; i += 1
                if c == left[c]:
                    if len(out) >= size: return None
                    out.append(c)
                else:
                    if len(stack) >= 254: return None
                    stack.append(right[c]); stack.append(left[c])
        if len(out) != size: return None
        return bytes(out)
    except IndexError:
        return None

def texfile_entries(b, hdr):
    """dc_tex texfile(): returns list of entry offsets (absolute in b) or None"""
    n = len(b)
    if n < 0x20 or hdr > n - 0x20: return None
    count = rd32(b, hdr); ent = rd32(b, hdr + 4) * 4
    if count == 0 or count > 1024 or ent < 0x10 or ent > n or (n - ent) // 0x40 < count: return None
    ok = 0
    out = []
    for i in range(count):
        e = ent + i * 0x40
        pix = rd32(b, e) * 4; clut = rd32(b, e + 4) * 4; size = rd32(b, e + 8); csize = rd32(b, e + 12)
        t0 = struct.unpack_from('<Q', b, e + 0x30)[0]
        out.append(e)
        if pix == 0 and clut == 0: continue
        if pix > n or size > n - pix or clut > n or csize > n - clut or ((t0 >> 26) & 15) > 10 or ((t0 >> 30) & 15) > 10 \
           or psm_bits((t0 >> 20) & 63) == 0 or (pix != 0 and size != 0 and not upload_ok(b, pix, size)):
            return None
        ok += pix != 0 and size != 0
    if ok == 0: return None
    return out

def is_font(b):
    n = len(b)
    if n < 0x24 or b[:4] != b'5tnF' or rd32(b, 0xC) > n - 0x40: return None
    return rd32(b, 0xC)

def walk(b, path, depth=0, emit=None):
    """emit(kind, b, path, extra): kind in 'tex' (extra = (hdr, entries)), 'font' (extra = entry off), 'leaf'"""
    n = len(b)
    if n < 16 or depth > 6:
        emit('leaf', b, path, None); return
    ents = texfile_entries(b, 0)
    if ents is not None:
        emit('tex', b, path, (0, ents)); return
    fo = is_font(b)
    if fo is not None:
        emit('font', b, path, fo); return
    if rd32(b, 4) <= n - 8 and rd32(b, 4) + 8 + 2048 > n and rd32(b, 0) < (64 << 20):
        raw = bpe(b)
        if raw is not None:
            walk(raw, path + '/bpe', depth + 1, emit); return
    count = rd32(b, 0)
    i = 0
    while count != 0 and count <= 0x10000 and 4 + count * 4 <= n and i < count:
        o = rd32(b, 4 + i * 4) & ~3
        if o != 0 and (o < 4 + count * 4 or o > n): break
        i += 1
    if count == 0 or count > 0x10000 or 4 + count * 4 > n or i < count:
        # format of its own: search for texture tables (as dc_tex search())
        found = []
        import numpy as np
        m = (n - 4) // 4
        a = np.frombuffer(b[:m * 4 + 4], dtype='<u4').astype(np.int64)
        c_ = a[:-1]; e_ = a[1:] * 4
        hs = np.arange(m, dtype=np.int64) * 4
        ok = (c_ >= 1) & (c_ <= 1024) & (e_ >= 0x10) & (hs + 0x20 <= n)
        ok &= ((e_ <= n - hs) & ((n - hs - e_) // 0x40 >= c_)) | ((e_ <= n) & ((n - e_) // 0x40 >= c_))
        for h in (np.nonzero(ok)[0] * 4).tolist():
            c = rd32(b, h); ent = rd32(b, h + 4) * 4
            if c == 0 or c > 1024 or ent < 0x10: continue
            if ent <= n - h and (n - h - ent) // 0x40 >= c:
                e2 = texfile_entries(b[h:], 0)
                if e2 is not None:
                    found.append(('rel', h, e2)); continue
            if ent <= n and (n - ent) // 0x40 >= c:
                e2 = texfile_entries(b, h)
                if e2 is not None:
                    found.append(('abs', h, e2))
        for kind, h, e2 in found:
            if kind == 'rel':
                emit('tex', b[h:], path + '@%X' % h, (0, e2))
            else:
                emit('tex', b, path + '@%X' % h, (h, e2))
        emit('leaf', b, path, found)
        return
    offs = [rd32(b, 4 + j * 4) & ~3 for j in range(count)]
    for j in range(count):
        o = offs[j]
        if o == 0 or o == n: continue
        if j + 1 < count and offs[j + 1] == o: continue
        end = n
        for q in offs:
            if o < q < end: end = q
        sub = path + '/%d' % (j + 1)
        e2 = texfile_entries(b, o)
        if e2 is not None:
            emit('tex', b, sub + '(abs)', (o, e2))
        else:
            walk(b[o:end], sub, depth + 1, emit)

def files(root):
    for d in ('pzs3us0', 'pzs3us1', 'pzs3us2'):
        dd = os.path.join(root, d)
        for name in sorted(os.listdir(dd)):
            idx = int(name.split('.')[0])
            fid = idx + 1 if d == 'pzs3us1' else idx + 0xD48 if d == 'pzs3us2' else 0
            yield d + '/' + name, os.path.join(dd, name), fid
