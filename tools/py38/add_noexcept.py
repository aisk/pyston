"""Insert PYSTON_NOEXCEPT into every PyAPI_FUNC declaration of a CPython header."""
import re
import sys

TRAILING_ATTR = re.compile(r'\s*(Py_GCC_ATTRIBUTE\(\(.*\)\)|Py_DEPRECATED\([^)]*\)|_Py_NO_RETURN|Py_NO_INLINE)\s*$', re.S)


def process(text):
    out = []
    i = 0
    for m in re.finditer(r'PyAPI_FUNC\(', text):
        start = m.start()
        if start < i:
            continue
        # Find the end of the declaration: the first ';' or '{' at paren depth 0.
        depth = 0
        j = m.end() - 1
        while j < len(text):
            c = text[j]
            if c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
            elif depth == 0 and c in ';{':
                break
            j += 1
        if j >= len(text) or text[j] == '{':
            continue
        decl = text[start:j]
        if 'PYSTON_NOEXCEPT' in decl or '\n#' in decl:
            continue
        tail = ''
        while True:
            t = TRAILING_ATTR.search(decl)
            if not t:
                break
            tail = decl[t.start():] + tail
            decl = decl[:t.start()]
        out.append(text[i:start])
        out.append(decl.rstrip() + ' PYSTON_NOEXCEPT' + tail)
        i = j
    out.append(text[i:])
    return ''.join(out)


for fn in sys.argv[1:]:
    with open(fn) as f:
        text = f.read()
    new = process(text)
    if new != text:
        with open(fn, 'w') as f:
            f.write(new)
