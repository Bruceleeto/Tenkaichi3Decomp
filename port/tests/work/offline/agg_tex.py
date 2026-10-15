"""Aggregate tex_check.py's OUT/tex.pkl against tex.pak's index, as gs_pvr.c nat_get / Port_PalNative would look
them up. Target: no MISS, no shape difference, every pack entry used.  python3 -I agg_tex.py"""
import sys, os, pickle, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tex_check as T

res = pickle.load(open(os.path.join(T.OUT, 'tex.pkl'), 'rb'))
pak = T.load_pak()
idx = {int(k): i for i, k in enumerate(pak['key'])}
print('pak', T.PAK, 'entries', len(pak))
C = collections.Counter
stat = C(); miss = collections.defaultdict(list); used = set(); uniq = collections.defaultdict(set)
psmc = C()
for rel, (fid, out) in res.items():
    for path, i, kind, psm, tw, th, key, dpsm, rw, rh in out:
        if kind in ('tex', 'fonttex'):
            psmc[psm] += 1
            if key is None:
                r = 'tex: packet not keyable'
            elif psm in (0, 1, 2, 0xA):
                k = idx.get(key ^ T.KEY_DIRECT)
                if k is None: r = 'tex direct: MISS'
                else:
                    e = pak[k]; used.add(k)
                    r = 'tex direct: hit' if (e['w'], e['h'], e['bits']) == (max(tw, 8), max(th, 8), 16) else 'tex direct: hit, shape differs'
            elif psm in (0x13, 0x14):
                k = idx.get(key)
                if k is None: r = 'tex indexed: MISS'
                else:
                    e = pak[k]; used.add(k)
                    r = 'tex indexed: hit' if (e['w'], e['h'], e['bits']) == (max(tw, 8), max(th, 8), 4 if psm == 0x14 else 8) else 'tex indexed: hit, shape differs'
            else:
                r = 'tex psm %X: not handled by nat_get' % psm
            uniq[r].add(key)
            if key is not None and psm in (0, 1, 2, 0xA) and idx.get(key ^ T.KEY_T8) is not None:
                used.add(idx[key ^ T.KEY_T8])  # (its 8-bit view, dc_tex.c emit_direct)
        else:
            kp = 'entrypal' if kind == 'entrypal' else 'pal'
            if key is None:
                r = kp + ': packet not keyable'
            else:
                k = idx.get(key)
                if k is None: r = kp + ': MISS'
                else:
                    e = pak[k]; used.add(k)
                    r = kp + ': hit' if e['bits'] == 32 and e['h'] == 1 and e['psm'] & 63 == dpsm and e['w'] == rw * rh else kp + ': hit, shape differs (bits %d w %d h %d psm %X dpsm %X)' % (e['bits'], e['w'], e['h'], e['psm'], dpsm)
            uniq[r].add(key)
        stat[r] += 1
        if 'MISS' in r or 'differs' in r or 'not' in r:
            miss[r].append('%s #%d psm %X %dx%d' % (path, i, psm, tw, th))
print('PSM of entries', {hex(k): v for k, v in psmc.items()})
for r, n in sorted(stat.items()):
    print('%-60s %7d entries %7d unique' % (r, n, len(uniq[r])))
for r, l in miss.items():
    print(r, len(l)); [print('   ', x) for x in l[:25]]
print('pak entries used', len(used), 'of', len(pak))
