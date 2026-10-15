"""The fighters' material palettes as the game uploads them: BtlObjMdl_InitAlphaSlots (src/sys/gfxm_e_b.c) writes
the alpha of the first 16 colours of each material texture's CLUT packet (bytes 0x60 + j*4 + 3) as
slot + row*15, row = the model's alphaRow (BtlObjVis_AllocFreeBit: 0..14). For every pmdl file, every material
entry and every row, the rewritten packet is keyed as gs_pvr.c Port_PalNative keys it (pkt_key, the 32-bit
palette's first 16 alphas left out) and looked up in tex.pak; the native palette Port_PalNative makes (the pack's
colours, pal_alphas: the packet's first 16 alphas) is compared with the colours the packet uploads.
Target: 0 misses, 0 differences.  python3 -I alpha_slots.py [FILE...]   (paths as tex_check.py: BT3_GAMEDATA, BT3_TEXPAK)"""
import sys, os, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bt3walk, tex_check as T

ROOT = os.path.join(T.ROOT, 'pzs3us1')
pak = T.load_pak()
idx = {int(k): i for i, k in enumerate(pak['key'])}
pakf = open(T.PAK, 'rb')


def pak_colours(k):
    e = pak[k]
    pakf.seek(int(e['off']))
    return list(struct.unpack('<%dI' % int(e['w']), pakf.read(int(e['w']) * 4)))


def packet_colours(p, img, rw, rh):
    """The colours a 16-colour (8 x 2) CT32 upload puts in GS memory, in index order (= image order)."""
    return list(struct.unpack_from('<%dI' % (rw * rh), p, img))


st = collections.Counter(); bad = []
files = sorted(os.listdir(ROOT)) if len(sys.argv) < 2 else sys.argv[1:]
for name in files:
    b = open(os.path.join(ROOT, name), 'rb').read()
    if len(b) < 0x40: continue
    cnt = bt3walk.rd32(b, 0)
    if cnt < 3 or cnt > 0x1000 or 4 + cnt * 4 > len(b): continue
    offs = [bt3walk.rd32(b, 4 + i * 4) for i in range(cnt)]
    if any(o > len(b) for o in offs) or offs[2] == 0 or b[offs[2]:offs[2] + 4] != b'pmdl':
        continue
    st['pmdl files'] += 1
    m = offs[2]
    first, count, noalpha = b[m + 0x4E], b[m + 0x4F], b[m + 0x54]
    tb = ents = None
    for pi, o in enumerate(offs):  # (the first texture table with enough entries: audit O4)
        if pi == 2 or o == 0: continue
        end = min([x for x in offs + [len(b)] if x > o])
        s_ = b[o:end]; e_ = bt3walk.texfile_entries(s_, 0)
        if e_ is not None and len(e_) >= first + count:
            tb, ents = s_, e_; st['texture part %d' % (pi + 1)] += 1; break
    if ents is None:
        st['no texture part'] += 1; continue
    for i, e in enumerate(ents):
        if not (first <= i < first + count): continue
        st['material entries'] += 1
        if noalpha != 0 and i == noalpha:
            st['  texNoAlpha (not rewritten)'] += 1; continue
        clut = bt3walk.rd32(tb, e + 4) * 4; csize = bt3walk.rd32(tb, e + 12); cblt = bt3walk.rd32(tb, e + 0x1C)
        slot = tb[e + 0x28 + 4]
        if not clut or not csize:
            st['  no clut'] += 1; continue
        st['  slot %2d' % slot] += 1
        ok, k0, rw, rh, img = T.pkt_key(tb, clut, csize, cblt, True)
        st['  palette CPSM %X, %d x %d, image data at packet + 0x%X' % ((cblt >> 8) & 63, rw, rh, img - clut)] += 1
        st['  file alphas of the first 16: %s' % sorted(set(tb[img + j * 4 + 3] for j in range(16)))] += 1
        misses = diffs = 0
        for row in range(15):
            p = bytearray(tb[clut:clut + csize])
            for j in range(16): p[0x60 + j * 4 + 3] = (slot + row * 15) & 0xFF
            ok, k, rw, rh, im = T.pkt_key(bytes(p), 0, csize, cblt, True)
            st['  rows keyed as the file\'s palette: %s' % (k == k0)] += 1
            if not ok or k not in idx:
                misses += 1; continue
            col = pak_colours(idx[k])
            if len(col) != rw * rh: diffs += 1; continue
            for j in range(16):  # (Port_PalNative pal_alphas; 16 colours: index = image order)
                col[j] = (col[j] & 0xFFFFFF) | p[im + j * 4 + 3] << 24
            if col != packet_colours(bytes(p), im, rw, rh): diffs += 1
        st['  rows missing the pack: %d of 15' % misses] += 1
        st['  rows whose native palette differs from the upload: %d of 15' % diffs] += 1
        if (misses or diffs) and len(bad) < 10: bad.append((name, i, slot, misses, diffs))
for k, v in sorted(st.items()): print('%-70s %d' % (k, v))
print('bad', bad)
