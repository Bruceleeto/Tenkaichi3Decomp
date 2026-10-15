"""Runs port/tests/work/vu1_check (built as its header says) over every file of the archives that has chains of a
program in the given part, and sums its results: the triangles compared, the worst corner / depth / s t q error,
the differences.

    nice -n 19 python3 -I port/tests/work/vu1_all.py <vu1_check binary> <program> <part> [workers] [env=value ...]
    (program as vu1_check takes it; part 3 of a pmdl file for the fighters, 1 or 9 of a stage file; at most 2 workers)
"""
import os, re, struct, subprocess, sys
from multiprocessing import Pool

ROOTS = ['gamedata/pzs3us0', 'gamedata/pzs3us1']
LINE = re.compile(r'(\d+) batches; (\d+) triangles from the microcode \((\d+) of them clipped; (\d+) left out\), (\d+) from records; '
                  r'worst corner ([\d.]+) px, depth (\d+), s t q ([\d.e+-]+) relative; (\d+) differences')
CHAINS = re.compile(r'program \d+: (\d+) chains')


def files(part):
    """The files that are an offset table with that part (u32 count, count offsets), and a pmdl in part 3 when
    the part asked for is 3."""
    for root in ROOTS:
        for name in sorted(os.listdir(root)):
            path = os.path.join(root, name)
            with open(path, 'rb') as f:
                b = f.read()
            if len(b) < 0x40:
                continue
            cnt = struct.unpack_from('<I', b, 0)[0]
            if cnt < part or cnt > 0x1000 or 4 + cnt * 4 > len(b):
                continue
            offs = list(struct.unpack_from('<%dI' % cnt, b, 4))
            if any(o > len(b) for o in offs) or offs[part - 1] == 0:
                continue
            if part == 3 and b[offs[2]:offs[2] + 4] != b'pmdl':
                continue
            yield path


def run(job):
    exe, prog, part, path, env = job
    r = subprocess.run([exe, str(prog), path, str(part)], capture_output=True, text=True, env=env)
    m = LINE.search(r.stdout)
    c = CHAINS.search(r.stdout)
    return path, int(c.group(1)) if c else 0, m.groups() if m else None, r.stdout if r.returncode != 0 else ''


def main():
    exe, prog, part = os.path.abspath(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
    workers = min(int(sys.argv[4]) if len(sys.argv) > 4 else 2, 2)
    env = dict(os.environ)
    for kv in sys.argv[5:]:
        k, v = kv.split('=', 1)
        env[k] = v
    jobs = [(exe, prog, part, p, env) for p in files(part)]
    tot = [0] * 9
    worst = [0.0, 0.0, 0.0]
    nfiles = nchains = 0
    bad = []
    with Pool(workers) as pool:
        for path, chains, g, out in pool.imap_unordered(run, jobs, 8):
            if chains == 0:
                continue
            nfiles += 1
            nchains += chains
            if g is None:
                bad.append((path, out[-400:]))
                continue
            for i in (0, 1, 2, 3, 4, 8):
                tot[i] += int(g[i])
            for i, j in ((0, 5), (1, 6), (2, 7)):
                worst[i] = max(worst[i], float(g[j]))
            if int(g[8]) != 0 or out:
                bad.append((path, out[-400:]))
    print('program %d part %d: %d files, %d chains, %d batches (over 7 cameras); %d triangles from the microcode (%d clipped, '
          '%d left out), %d from records; worst corner %.4f px, depth %.0f, s t q %.3g relative; %d differences, %d files with any'
          % (prog, part, nfiles, nchains, tot[0], tot[1], tot[2], tot[3], tot[4], worst[0], worst[1], worst[2], tot[8], len(bad)))
    for path, out in bad[:10]:
        print('  ' + path + ':\n' + out)


if __name__ == '__main__':
    main()
