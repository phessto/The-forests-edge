#!/usr/bin/env python3
"""Undo the line-ending damage in the 2009 TFE area files.

The files in the 2009 zip went through a conversion that turned every LF
into CR LF and every lone CR into CR LF. TFE's own format is plain LF
between fields and LF CR at the end of each line inside "strings", so a
string line "text\\n\\r" became "text\\r\\n\\r\\n" and the game sent a blank
line after every line of every description and help page.

Outside strings: CR LF -> LF.
Inside strings: a newline run that is exactly (CR LF) x k is damaged, and
decodes to k characters alternating LF, CR, LF, ... ("\\n\\r" per line).
A damaged run followed by one stray CR (only in hatchery.are code) drops
that CR. Runs that start with LF are already in TFE's format and are left alone
(some files were re-saved by the game after the damage).

Usage: tools/fix-2009-line-endings.py FILE...          (report only)
       WRITE=1 tools/fix-2009-line-endings.py FILE... (rewrite in place)
"""
import os, re, sys

RUN = re.compile(rb"[\r\n]+")

def fix_string(s, stats):
    def sub(m):
        r = m.group(0)
        if len(r) >= 5 and r.endswith(b"\r") and r[:-1] == b"\r\n" * (len(r) // 2):
            r = r[:-1]          # a stray CR after the damaged run (hatchery.are code)
            stats["trailing_cr"] += 1
        if len(r) % 2 == 0 and r == b"\r\n" * (len(r) // 2):
            k = len(r) // 2
            stats["damaged"] += 1
            if k % 2:
                stats["odd"] += 1
            return (b"\n\r" * k)[:k]
        stats["native"] += 1
        return r
    return RUN.sub(sub, s)

def convert(data, stats):
    out = bytearray(); i = 0; n = len(data)
    while i < n:
        c = data[i]
        if c == 0x22:                                   # start of "string"
            j = i + 1
            while j < n and data[j] != 0x22:
                j += 2 if data[j] == 0x5C else 1        # skip \" and \\
            out += b'"' + fix_string(data[i+1:j], stats) + b'"'
            i = j + 1
        elif data.startswith(b"\r\n", i):
            out += b"\n"; i += 2
        elif c == 0x0D:
            stats["lone_cr"] += 1; i += 1               # stray CR between fields: whitespace, drop
        else:
            out.append(c); i += 1
    return bytes(out)

write = os.environ.get("WRITE") == "1"
total = dict(damaged=0, native=0, odd=0, lone_cr=0, trailing_cr=0, files=0)
for f in sys.argv[1:]:
    if os.path.isdir(f):
        continue
    data = open(f, "rb").read()
    if b"\r" not in data:
        continue
    st = dict(damaged=0, native=0, odd=0, lone_cr=0, trailing_cr=0)
    new = convert(data, st)
    total["files"] += new != data
    for k in st: total[k] += st[k]
    if st["native"] or st["lone_cr"]:
        print(f"{f}: {st}")
    if write and new != data:
        open(f, "wb").write(new)
print("total:", total)
