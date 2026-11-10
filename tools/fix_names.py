#!/usr/bin/python
# -*- coding: utf-8 -*-
# fix_names.py DIR [--dry-run]: repair file names that were mangled into "mojibake" (UTF-8 bytes
# read as Latin-1 and encoded again, once or twice - "ãã©ã­ãã" for "ヒラギノ"). An export
# archive written by tar in the C locale on Snow Leopard comes out that way on extraction: the
# Japanese and Chinese fonts then got names that Word 2004 reported as corrupt fonts.
# Names are only changed when undoing the extra encoding gives valid UTF-8 with non-Latin text.
import os, sys, unicodedata

def unmangle(name):
    u = name.decode('utf-8') if isinstance(name, str) else name
    u = unicodedata.normalize('NFC', u)
    changed = False
    for _ in range(3):
        try:
            b = u.encode('latin-1')          # only mangled names are all Latin-1
            v = b.decode('utf-8')
        except (UnicodeEncodeError, UnicodeDecodeError):
            break
        if v == u: break
        u = v; changed = True
    if not changed: return None
    if all(ord(c) < 0x100 for c in u): return None   # stayed Latin: probably a real Latin-1 name
    return unicodedata.normalize('NFD', u).encode('utf-8')

def main():
    top = sys.argv[1]; dry = '--dry-run' in sys.argv
    n = 0
    for root, dirs, files in os.walk(top, topdown=False):
        for nm in files + dirs:
            new = unmangle(nm)
            if not new or new == nm: continue
            src, dst = os.path.join(root, nm), os.path.join(root, new)
            if os.path.lexists(dst): print('exists, kept: %s' % src); continue
            print('%s -> %s' % (src, new))
            if not dry: os.rename(src, dst)
            n += 1
    print('%d name(s) %s' % (n, 'to fix' if dry else 'fixed'))

if __name__ == '__main__': main()
