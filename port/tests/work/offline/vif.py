"""DMA source chains (TTE: the tag's upper 64 bits are two VIF codes) and VIF1 streams, statically."""
import struct
import numpy as np

VIF_NAMES = {0x00: 'NOP', 0x01: 'STCYCL', 0x02: 'OFFSET', 0x03: 'BASE', 0x04: 'ITOP', 0x05: 'STMOD', 0x06: 'MSKPATH3',
             0x07: 'MARK', 0x10: 'FLUSHE', 0x11: 'FLUSH', 0x13: 'FLUSHA', 0x14: 'MSCAL', 0x15: 'MSCALF', 0x17: 'MSCNT',
             0x20: 'STMASK', 0x30: 'STROW', 0x31: 'STCOL', 0x4A: 'MPG', 0x50: 'DIRECT', 0x51: 'DIRECTHL'}
DMA_IDS = {0: 'refe', 1: 'cnt', 2: 'next', 3: 'ref', 4: 'refs', 5: 'call', 6: 'ret', 7: 'end'}
GS_REGS = {0x00: 'PRIM', 0x01: 'RGBAQ', 0x02: 'ST', 0x03: 'UV', 0x04: 'XYZF2', 0x05: 'XYZ2', 0x06: 'TEX0_1', 0x07: 'TEX0_2',
           0x08: 'CLAMP_1', 0x09: 'CLAMP_2', 0x0A: 'FOG', 0x0C: 'XYZF3', 0x0D: 'XYZ3', 0x0E: 'A+D', 0x0F: 'NOP',
           0x14: 'TEX1_1', 0x15: 'TEX1_2', 0x16: 'TEX2_1', 0x17: 'TEX2_2', 0x18: 'XYOFFSET_1', 0x19: 'XYOFFSET_2',
           0x1A: 'PRMODECONT', 0x1B: 'PRMODE', 0x1C: 'TEXCLUT', 0x22: 'SCANMSK', 0x34: 'MIPTBP1_1', 0x35: 'MIPTBP1_2',
           0x36: 'MIPTBP2_1', 0x37: 'MIPTBP2_2', 0x3B: 'TEXA', 0x3D: 'FOGCOL', 0x3F: 'TEXFLUSH', 0x40: 'SCISSOR_1',
           0x41: 'SCISSOR_2', 0x42: 'ALPHA_1', 0x43: 'ALPHA_2', 0x44: 'DIMX', 0x45: 'DTHE', 0x46: 'COLCLAMP',
           0x47: 'TEST_1', 0x48: 'TEST_2', 0x49: 'PABE', 0x4A: 'FBA_1', 0x4B: 'FBA_2', 0x4C: 'FRAME_1', 0x4D: 'FRAME_2',
           0x4E: 'ZBUF_1', 0x4F: 'ZBUF_2', 0x50: 'BITBLTBUF', 0x51: 'TRXPOS', 0x52: 'TRXREG', 0x53: 'TRXDIR',
           0x54: 'HWREG', 0x60: 'SIGNAL', 0x61: 'FINISH', 0x62: 'LABEL'}
PACKED_REGS = {0: 'PRIM', 1: 'RGBAQ', 2: 'ST', 3: 'UV', 4: 'XYZF2', 5: 'XYZ2', 6: 'TEX0_1', 7: 'TEX0_2', 8: 'CLAMP_1',
               9: 'CLAMP_2', 0xA: 'FOG', 0xC: 'XYZF3', 0xD: 'XYZ3', 0xE: 'A+D', 0xF: 'NOP'}
PRIM_TYPES = ['POINT', 'LINE', 'LINESTRIP', 'TRI', 'TRISTRIP', 'TRIFAN', 'SPRITE', 'PROHIBITED']


def vif_valid(w):
    c = (w >> 24) & 0x7F
    return c in VIF_NAMES or (c & 0x60) == 0x60


def unpack_words(cmd, num, cl, wl):
    vn = (cmd >> 2) & 3; vl = cmd & 3
    n = num if num else 256
    bits = 16 if (vn == 3 and vl == 3) else (32 >> vl) * (vn + 1)
    if wl > cl and wl != 0:
        n = cl * (n // wl) + (min(n % wl, cl))
    return (n * bits + 31) // 32


def gif_tag(lo, hi):
    nloop = lo & 0x7FFF; eop = (lo >> 15) & 1; pre = (lo >> 46) & 1; prim = (lo >> 47) & 0x7FF
    flg = (lo >> 58) & 3; nreg = (lo >> 60) & 15
    regs = [(hi >> (4 * i)) & 15 for i in range(nreg if nreg else 16)]
    return dict(nloop=nloop, eop=eop, pre=pre, prim=prim, flg=flg, nreg=nreg, regs=tuple(regs))


def prim_str(p):
    s = PRIM_TYPES[p & 7]
    f = []
    if p & 8: f.append('IIP')
    if p & 0x10: f.append('TME')
    if p & 0x20: f.append('FGE')
    if p & 0x40: f.append('ABE')
    if p & 0x80: f.append('AA1')
    if p & 0x100: f.append('FST')
    f.append('CTXT2' if p & 0x200 else 'CTXT1')
    if p & 0x400: f.append('FIX')
    return '0x%03X %s %s' % (p, s, '|'.join(f))


def tag_str(t):
    return 'NLOOP=%d EOP=%d PRE=%d PRIM=%s FLG=%d REGS=%s' % (
        t['nloop'], t['eop'], t['pre'], prim_str(t['prim']) if t['pre'] else '-', t['flg'],
        ','.join(PACKED_REGS.get(r, '?%X' % r) for r in t['regs']))


def candidates(b):
    """16-aligned offsets whose quadword could be a DMA tag with two VIF codes"""
    n = len(b) // 16
    if n == 0:
        return []
    a = np.frombuffer(b[:n * 16], dtype='<u4').reshape(n, 4)
    w0 = a[:, 0]; w2 = a[:, 2]; w3 = a[:, 3]
    ids = (w0 >> 28) & 7
    ok = ((w0 & 0x0FFF0000) == 0) & ((w0 >> 31) == 0)
    ok &= (ids == 1) | (ids == 6) | (ids == 7) | (ids == 2) | (ids == 3) | (ids == 5) | (ids == 0)
    ok &= ((w0 & 0xFFFF) > 0) | (ids == 6) | (ids == 7)
    for w in (w2, w3):
        c = (w >> 24) & 0x7F
        v = (c & 0x60) == 0x60
        for k in VIF_NAMES:
            v |= c == k
        ok &= v
    # at least one of the two codes not a plain NOP-with-zero, to cut noise (zero words are NOP)
    ok &= ~((w2 == 0) & (w3 == 0))
    return (np.nonzero(ok)[0] * 16).tolist()


class Stream:
    """A VIF stream assembled from a DMA chain."""

    def __init__(self):
        self.words = []       # (word, file offset)
        self.tags = []        # (offset, id, qwc, addr)
        self.err = None


def chain(b, off, maxtags=100000):
    """follow the DMA chain at off; returns Stream (words of the VIF stream, tags) and end offset"""
    s = Stream()
    n = len(b)
    at = off
    stack = 0
    for _ in range(maxtags):
        if at + 16 > n:
            s.err = 'chain runs off the part'; return s, at
        w0, w1, w2, w3 = struct.unpack_from('<4I', b, at)
        qwc = w0 & 0xFFFF; tid = (w0 >> 28) & 7
        if (w0 & 0x0FFF0000) or not vif_valid(w2) or not vif_valid(w3):
            s.err = 'bad tag at +0x%X (%08X %08X %08X %08X)' % (at - off, w0, w1, w2, w3); return s, at
        s.tags.append((at, tid, qwc, w1))
        s.words.append((w2, at + 8)); s.words.append((w3, at + 12))
        if tid in (0, 3, 4):     # data at ADDR (file has absolute addresses: cannot follow)
            s.err = 'tag %s with address 0x%X' % (DMA_IDS[tid], w1); return s, at + 16
        data = at + 16
        if data + qwc * 16 > n:
            s.err = 'tag data runs off the part'; return s, at
        for k in range(qwc * 4):
            s.words.append((struct.unpack_from('<I', b, data + k * 4)[0], data + k * 4))
        nxt = data + qwc * 16
        if tid == 1:
            at = nxt; continue
        if tid == 7:
            return s, nxt
        if tid == 6:
            if stack == 0:
                return s, nxt
            s.err = 'ret inside call'; return s, nxt
        if tid in (2, 5):
            s.err = 'tag %s with address 0x%X' % (DMA_IDS[tid], w1); return s, nxt
    s.err = 'too many tags'
    return s, at


def parse_vif(words):
    """returns list of commands: dict(cmd, name, num, imm, data=[(word, off)...]) or raises ValueError"""
    out = []
    i = 0
    cl = wl = 1
    n = len(words)
    while i < n:
        w, off = words[i]
        cmd = (w >> 24) & 0x7F; num = (w >> 16) & 0xFF; imm = w & 0xFFFF
        i += 1
        c = dict(cmd=cmd, num=num, imm=imm, off=off, irq=w >> 31)
        if (cmd & 0x60) == 0x60:
            nw = unpack_words(cmd, num, cl, wl)
            if i + nw > n:
                raise ValueError('UNPACK data runs off the stream at 0x%X' % off)
            c['name'] = 'UNPACK'
            c['data'] = words[i:i + nw]; i += nw
            c['cl'] = cl; c['wl'] = wl
        elif cmd in VIF_NAMES:
            c['name'] = VIF_NAMES[cmd]
            if cmd == 0x01:
                cl = imm & 0xFF; wl = (imm >> 8) & 0xFF
            elif cmd == 0x20:
                c['data'] = words[i:i + 1]; i += 1
            elif cmd in (0x30, 0x31):
                c['data'] = words[i:i + 4]; i += 4
            elif cmd == 0x4A:
                nw = (num if num else 256) * 2
                c['data'] = words[i:i + nw]; i += nw
            elif cmd in (0x50, 0x51):
                # aligned: data starts at the next quadword boundary of the stream
                while i < n and (words[i][1] & 15) != 0:
                    i += 1
                nw = (imm if imm else 65536) * 4
                if i + nw > n:
                    raise ValueError('DIRECT data runs off at 0x%X' % off)
                c['data'] = words[i:i + nw]; i += nw
        else:
            raise ValueError('unknown VIF code %08X at 0x%X' % (w, off))
        out.append(c)
    return out


def qws(data):
    """V4-32 data words -> list of 4-word tuples"""
    w = [x for x, _ in data]
    return [tuple(w[k:k + 4]) for k in range(0, len(w) - 3, 4)]
