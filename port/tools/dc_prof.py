#!/usr/bin/env python3
"""The Dreamcast's profile (make -f Makefile.dc DC_PROF=1, port/src/dc/dc_prof.c) as functions.

    port/tools/dc_prof.py <log> [dump number | all] [--elf port/build/dc/bt3.elf] [--top 40]

<log> is the program's output (Flycast's serial console / log), with its "prof:" lines. Without a dump number the
last whole one is shown; "all" adds them up. Each bin's samples go to the function its address is in (sh-elf-nm);
the list is the share of the time spent in each function itself (not in what it calls). Then the callers of
memcpy / memset ("prof: c" lines: return addresses), by function and source line (sh-elf-addr2line)."""
import argparse
import bisect
import re
import subprocess
import sys

NM = "/opt/toolchains/dc/sh-elf/bin/sh-elf-nm"
A2L = "/opt/toolchains/dc/sh-elf/bin/sh-elf-addr2line"


def dumps(path):
    """{dump: (header, {offset: count}, {caller: count})} of the whole dumps in the log"""
    out, cur, head = {}, None, None
    with open(path, errors="replace") as f:
        for line in f:
            i = line.find("prof: ")
            if i < 0:
                continue
            line = line[i + 6:].strip()
            m = re.match(r"dump (\d+): (.*)", line)
            if m:
                cur, head = int(m.group(1)), m.group(2)
                bins, calls = {}, {}
                continue
            if cur is None:
                continue
            if line.startswith("b "):
                for kv in line[2:].split():
                    k, v = kv.split("=")
                    bins[int(k, 16)] = bins.get(int(k, 16), 0) + int(v)
            elif line.startswith("c "):
                for kv in line[2:].split():
                    k, v = kv.split("=")
                    calls[int(k, 16)] = calls.get(int(k, 16), 0) + int(v)
            elif line.startswith("copies "):
                print("prof:", line)
            elif line.startswith("end "):
                out[cur] = (head, bins, calls)
                cur = None
            elif line.startswith("on:"):
                print("prof:", line)
    return out


def symbols(elf):
    """sorted (address, name) of the code symbols"""
    syms = []
    for line in subprocess.run([NM, "-n", elf], capture_output=True, text=True, check=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in "TtWw":
            syms.append((int(p[0], 16), p[2]))
    return syms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("dump", nargs="?", default=None)
    ap.add_argument("--elf", default="port/build/dc/bt3.elf")
    ap.add_argument("--top", type=int, default=40)
    a = ap.parse_args()

    d = dumps(a.log)
    if not d:
        sys.exit("no whole prof dump in " + a.log)
    if a.dump == "all":
        pick = sorted(d)
    else:
        pick = [int(a.dump) if a.dump is not None else max(d)]
    bins, calls = {}, {}
    for n in pick:
        print("dump %d: %s" % (n, d[n][0]))
        for k, v in d[n][1].items():
            bins[k] = bins.get(k, 0) + v
        for k, v in d[n][2].items():
            calls[k] = calls.get(k, 0) + v

    syms = symbols(a.elf)
    addrs = [s[0] for s in syms]
    base = next(s[0] for s in syms if s[1] == "__executable_start") if any(s[1] == "__executable_start" for s in syms) else 0x8C010000
    per = {}
    for off, n in bins.items():
        i = bisect.bisect_right(addrs, base + off) - 1
        name = syms[i][1] if i >= 0 else "?"
        per[name] = per.get(name, 0) + n
    total = sum(per.values())
    print("%d samples in the code" % total)
    print("%7s %6s  %s" % ("samples", "%", "function"))
    for name, n in sorted(per.items(), key=lambda x: -x[1])[: a.top]:
        print("%7d %5.1f%%  %s" % (n, 100.0 * n / total, name))
    if not calls:
        return
    top = sorted(calls.items(), key=lambda x: -x[1])[: a.top]
    a2l = subprocess.run([A2L, "-f", "-e", a.elf] + ["%x" % (c - 2) for c, _ in top], capture_output=True, text=True,
                         check=True).stdout.splitlines()  # (pr - 2: the call itself, not the line after it)
    ctotal = sum(calls.values())
    print("\nthe callers of memcpy / memset: %d samples" % ctotal)
    print("%7s %6s  %-10s %-32s %s" % ("samples", "%", "address", "function", "line"))
    for i, (c, n) in enumerate(top):
        fn, ln = a2l[2 * i], a2l[2 * i + 1].split("/")[-1]
        print("%7d %5.1f%%  %08x   %-32s %s" % (n, 100.0 * n / ctotal, c, fn, ln))


main()
