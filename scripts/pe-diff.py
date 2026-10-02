"""Which parts of several builds of one PE program differ: the headers, each
section (long names resolved through the COFF string table) and whatever
follows the last section.

usage: pe-diff.py <a.exe> <b.exe> ...
"""

import hashlib
import struct
import sys


def parts(path):
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    _, count, _, symtab, nsyms, opt_size, _ = struct.unpack_from("<HHIIIHH", data, pe + 4)
    table = pe + 24 + opt_size
    strings = symtab + nsyms * 18 if symtab else None
    out = {"headers": data[:table + 40 * count]}
    end = table + 40 * count
    for i in range(count):
        raw = data[table + 40 * i:table + 40 * (i + 1)]
        name = raw[:8].rstrip(b"\0").decode()
        if name.startswith("/") and strings is not None:
            offset = strings + int(name[1:])
            name = data[offset:data.index(b"\0", offset)].decode()
        size, pointer = struct.unpack_from("<II", raw, 16)
        out[name] = data[pointer:pointer + size]
        end = max(end, pointer + size)
    out["(after sections)"] = data[end:]
    return out, len(data)


files = sys.argv[1:]
builds = [parts(f) for f in files]
print("sizes:", " ".join("%s=%d" % (f.rsplit("/", 1)[-1], n) for f, (_, n) in zip(files, builds)))
for name in builds[0][0]:
    blobs = [b[0].get(name, b"") for b in builds]
    digests = [hashlib.sha256(x).hexdigest()[:8] for x in blobs]
    if len(set(digests)) == 1:
        continue
    first = blobs[0]
    where = ""
    for other in blobs[1:]:
        if other != first:
            n = next((i for i, (a, b) in enumerate(zip(first, other)) if a != b), min(len(first), len(other)))
            diff = sum(1 for a, b in zip(first, other) if a != b)
            where = "first difference at +%#x, %d bytes differ, sizes %d/%d" % (n, diff, len(first), len(other))
            break
    print("%-20s %s  %s" % (name, " ".join(digests), where))
