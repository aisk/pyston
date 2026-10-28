# Hot int and float loops, which the LLVM tier can keep unboxed.
import sys

def overflow(n):
    s = 1
    for i in range(n):
        s = s * 3 + i
    return s

def lcg(n):
    x = 1
    for i in range(n):
        x = (x * 1103515245 + 12345) & 0x7fffffff
    return x

def mixed_compare(n):
    big = 2 ** 53 + 1
    c = 0
    for i in range(n):
        x = big + i * 0
        if x == 9007199254740992.0:
            c += 1
        if x > 9007199254740992.0:
            c += 2
    return c

def floats(n):
    zr = zi = 0.0
    s = 0.0
    for i in range(n):
        t = zr * zr - zi * zi + 0.25
        zi = 2.0 * zr * zi + i / n
        zr = t
        s += zr - zi + i * 0.5
        if s > 1e9:
            s = 0.0
    return s

def introspect(n):
    s = 0
    f = 1.5
    seen = []
    for i in range(n):
        s += i
        f *= 1.0
        if i == n - 1:
            seen.append(sorted(locals().items()))
            seen.append(sys._getframe().f_locals["s"])
    return seen

def raises(n):
    s = 0
    for i in range(n):
        s += i
        if i == n - 1:
            try:
                1 / (s - s)
            except ZeroDivisionError as e:
                return s, type(e).__name__, e.__traceback__.tb_frame.f_locals["s"]

def while_loop(n):
    i = 0
    c = 0
    while i < n:
        if i & 3 == 1:
            c += 1
        i += 1
    return c

for n in [10, 5000, 30000]:
    print(overflow(n) % 1000000007, lcg(n), mixed_compare(n), round(floats(n), 6), while_loop(n))
    print(introspect(n), raises(n))
print(overflow(200) == overflow(200), overflow(100) > 2 ** 64)
