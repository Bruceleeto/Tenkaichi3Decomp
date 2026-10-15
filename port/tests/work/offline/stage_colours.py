"""The stage geometry's vertex colours (worklist 0.5): every DMA/VIF chain of the archives (as the offline audit's
scanner finds them), every batch with the 3-quadword stage header (programs 4 and 7: the stage and its debris;
program 8: the animated objects, header +0 a TEX0_1 tag), its vertices' colour quadword (floats, 0x80 = 1.0).
Per program family: the maximum per channel, and the batches with a colour channel over 128.
    python3 -I stage_colours.py [JOBS]   (paths as tex_check.py: BT3_GAMEDATA)"""
import sys, os, struct, collections, multiprocessing as mp
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bt3walk, vif, tex_check as T


def batches(b):
    """the vertex data of every 3-qw-header batch of every chain in b: (family, n x 3 x 4 float32 array)"""
    pos = 0
    for off in vif.candidates(b):
        if off < pos:
            continue
        s, end = vif.chain(b, off)
        try:
            cmds = vif.parse_vif(s.words)
        except ValueError:
            continue
        if len(cmds) < 2 or not any(c['name'] == 'MSCNT' for c in cmds) or s.err:
            continue
        pos = end
        hdr = verts = None
        for c in cmds:
            if c['name'] == 'UNPACK' and c['cmd'] & 0x6F == 0x6C:
                q = vif.qws(c['data'])
                if c['imm'] & 0x3FF == 0 and hdr is None:
                    hdr = q
                else:
                    verts = q
            elif c['name'] in ('MSCAL', 'MSCALF', 'MSCNT'):
                if hdr is not None and len(hdr) == 3 and verts and len(verts) % 3 == 0:
                    regs = hdr[0][2] & 0xF  # (the +0 tag's first register: NOP for the stage, TEX0_1 for an animated node)
                    a = np.array(verts, dtype='<u4').view('<f4').reshape(-1, 3, 4)
                    yield ('prog8 (animated objects)' if regs == 6 else 'prog4/7 (stage, debris)'), a
                hdr = verts = None


def do_file(args):
    rel, full, fid = args
    with open(full, 'rb') as f:
        b = f.read()
    if len(b) < 0x30 or b[0x20:0x26] == b'(c)CRI' or (b[0] == 0x80 and b[1] == 0):
        return rel, None
    R = {}

    def emit(kind, data, path, extra):
        if kind != 'leaf':
            return
        for fam, a in batches(data):
            r = R.setdefault(fam, dict(batches=0, verts=0, max=np.zeros(4, np.float32), over=0, overv=0, alpha=collections.Counter()))
            col = a[:, 1, :]
            r['batches'] += 1; r['verts'] += len(col)
            r['max'] = np.maximum(r['max'], col.max(axis=0))
            o = (col[:, :3] > 128.0).any(axis=1)
            r['over'] += int(o.any()); r['overv'] += int(o.sum())
            r['alpha'].update(np.round(col[:, 3], 1).tolist())
    bt3walk.walk(b, rel, 0, emit)
    return rel, R


def main():
    jobs = [j for j in bt3walk.files(T.ROOT) if not j[0].startswith('pzs3us2')]
    tot = {}
    hist = collections.defaultdict(collections.Counter)
    bright = collections.defaultdict(list)
    with mp.Pool(int(sys.argv[1]) if len(sys.argv) > 1 else 2) as pool:
        for rel, R in pool.imap_unordered(do_file, jobs, chunksize=4):
            for fam, r in (R or {}).items():
                t = tot.setdefault(fam, dict(batches=0, verts=0, max=np.zeros(4, np.float32), over=0, overv=0, files=0, alpha=collections.Counter()))
                for k in ('batches', 'verts', 'over', 'overv'):
                    t[k] += r[k]
                t['files'] += 1
                t['max'] = np.maximum(t['max'], r['max'])
                t['alpha'].update(r['alpha'])
                hist[fam][int(r['max'][:3].max())] += 1
                if r['max'][:3].max() > 255.0:
                    bright[fam].append('%s %.1f' % (rel, r['max'][:3].max()))
    for fam, t in sorted(tot.items()):
        print('%s: %d files, %d batches, %d vertices' % (fam, t['files'], t['batches'], t['verts']))
        print('   maximum r g b a: %.2f %.2f %.2f %.2f' % tuple(t['max']))
        print('   batches with a colour channel over 128: %d (%d vertices)' % (t['over'], t['overv']))
        print('   alpha values (most common):', t['alpha'].most_common(12))
        print('   files by their brightest channel:', sorted(hist[fam].items())[-12:])
        print('   files over 255:', sorted(bright[fam]))


if __name__ == '__main__':
    main()
