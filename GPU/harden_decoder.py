"""Add packet bounds and null-dispatch checks to pinned emugen output.

Wire offsets use size_t arithmetic. Reject a bad connection before any GL
call; never let a guest-declared pointer extent escape the current packet.
"""
from pathlib import Path
import re
import sys

path = Path(sys.argv[1])
source = path.read_text()
source = source.replace('uint32_t opcode = *(uint32_t *)ptr;',
                        'uint32_t opcode = Unpack<uint32_t,uint32_t>(ptr);')
source = source.replace('size_t packetLen = *(uint32_t *)(ptr + 4);',
                        'size_t packetLen = Unpack<uint32_t,uint32_t>(ptr + 4);')
source = source.replace('if (len - pos < packetLen)',
                        'if (packetLen < 8 || len - pos < packetLen)')
source = re.sub(r'(case OP_(\w+): \{)', r'\1\n            if (!this->\2) return 0;', source)
lines = []
for line in source.splitlines():
    access = re.search(r'Unpack<[^,]+,uint(\d+)_t>\(ptr \+ (.+)\);', line)
    if access and 'size_t packetLen =' not in line:
        bits, offset = access.groups()
        offset = re.sub(r'^8\b', 'size_t(8)', offset)
        line = re.sub(r'ptr \+ 8\b', 'ptr + size_t(8)', line)
        lines.append(f'            if ({offset} > packetLen || packetLen - ({offset}) < {int(bits)//8}) return 0;')
    extent = re.search(r'InputBuffer inptr_(\w+)\(ptr \+ (.+), size_\1\);', line)
    if extent:
        name, offset = extent.groups()
        offset = re.sub(r'^8\b', 'size_t(8)', offset)
        lines.append(f'            if ({offset} > packetLen || size_{name} > packetLen - ({offset})) return 0;')
    length = re.search(r'uint32_t size_(\w+).* = Unpack', line)
    lines.append(line)
    if length:
        lines.append(f'            if (size_{length[1]} > 64u * 1024u * 1024u) return 0;')
source = '\n'.join(lines) + '\n'
# Guest strings/output lengths can put scalar return values at unaligned offsets.
source = re.sub(r'\*\(([^)]+) \*\)\(&tmpBuf\[([^\]]+)\]\) = ([^;]+);',
    lambda m: f'{m[1]} returnValue = {m[3]}; memcpy(&tmpBuf[{m[2]}], &returnValue, sizeof(returnValue));', source)
path.write_text(source)
