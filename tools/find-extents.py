#!/usr/bin/env python3
"""Find where a file's 4 KiB blocks live inside a disk image (by content), then
check which of them the phone actually read (from <image>.requests.log).

usage: find-extents.py <image> <file extracted from the image> [requests.log]
Prints, for every 4 KiB chunk of the file: its image offset, and whether a read
request covered it. A chunk the phone read from the wrong place shows up as
"NOT READ" while some other offset nearby was read instead.
"""
import sys

BS = 4096


def main():
    img, path = sys.argv[1], sys.argv[2]
    logp = sys.argv[3] if len(sys.argv) > 3 else img + '.requests.log'
    data = open(path, 'rb').read()
    chunks = {}
    for i in range(0, len(data), BS):
        c = data[i:i + BS].ljust(BS, b'\0')
        if c.count(0) != BS:
            chunks.setdefault(c, []).append(i)
    found = {}
    with open(img, 'rb') as f:
        off = 0
        while True:
            blk = f.read(64 << 20)
            if not blk:
                break
            for j in range(0, len(blk), BS):
                c = blk[j:j + BS]
                if c in chunks:
                    for fo in chunks[c]:
                        found.setdefault(fo, off + j)
            off += len(blk)
            if len(found) == sum(len(v) for v in chunks.values()):
                break
    reqs = []
    try:
        for line in open(logp):
            s, op, o, n = line.split()
            if op == '1':
                reqs.append((int(o, 16), int(n)))
    except FileNotFoundError:
        pass
    for fo in sorted(found):
        io = found[fo]
        hit = any(o <= io < o + n for o, n in reqs)
        print(f'file {fo:#08x} -> image {io:#012x} {"read" if hit else "NOT READ" if reqs else ""}')
    missing = sorted(set(sum(chunks.values(), [])) - set(found))
    if missing:
        print('chunks not found in image:', [hex(m) for m in missing[:20]])


if __name__ == '__main__':
    main()
