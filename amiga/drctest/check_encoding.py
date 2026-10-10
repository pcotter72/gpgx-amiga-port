"""Assemble the translator's intended text with m68k-amigaos-as and compare
it byte for byte with what it emitted (see drctest.c).

usage: python check_encoding.py out.bin out.s
"""
import os
import re
import subprocess
import sys
import tempfile

AS = 'm68k-amigaos-as'
OBJCOPY = 'm68k-amigaos-objcopy'


def main():
    binf, srcf = sys.argv[1], sys.argv[2]
    emitted = open(binf, 'rb').read()
    tmp = tempfile.mkdtemp()
    obj = os.path.join(tmp, 't.o')
    raw = os.path.join(tmp, 't.bin')
    r = subprocess.run([AS, '-m68000', '--register-prefix-optional', '-o', obj, srcf],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr[:4000])
        sys.exit('assembler failed')
    subprocess.run([OBJCOPY, '-O', 'binary', '-j', '.text', obj, raw], check=True)
    assembled = open(raw, 'rb').read()
    print('emitted %d bytes, assembled %d bytes' % (len(emitted), len(assembled)))

    # map byte offsets to (op, line) from the "| @pos" comments
    lines = open(srcf).read().split('\n')
    where = []
    op = None
    for ln in lines:
        m = re.match(r'\| op (\S+)', ln)
        if m:
            op = m.group(1)
            continue
        m = re.match(r'\t(.*)\t\| @(\d+)$', ln)
        if m:
            where.append((int(m.group(2)), op, m.group(1)))

    n = min(len(emitted), len(assembled))
    bad = 0
    for i in range(n):
        if emitted[i] != assembled[i]:
            # find the instruction containing byte i
            prev = None
            for w in where:
                if w[0] > i:
                    break
                prev = w
            print('mismatch at byte %d (op %s): "%s" emitted %s assembled %s'
                  % (i, prev[1] if prev else '?', prev[2] if prev else '?',
                     emitted[prev[0]:prev[0] + 8].hex() if prev else '',
                     assembled[prev[0]:prev[0] + 8].hex() if prev else ''))
            bad += 1
            if bad >= 20:
                break
    # the object file pads .text to a multiple of 4 with zero bytes
    if len(assembled) > len(emitted) and len(assembled) % 4 == 0 and \
            len(assembled) - len(emitted) < 4 and not any(assembled[len(emitted):]):
        assembled = assembled[:len(emitted)]
    if len(emitted) != len(assembled):
        print('LENGTH DIFFERS')
        bad += 1
    print('OK' if bad == 0 else 'FAILED')
    sys.exit(0 if bad == 0 else 1)


if __name__ == '__main__':
    main()
