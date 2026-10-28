# Binary operators reached from C code and from hot (JIT compiled) call sites.
import operator

# int arithmetic through the C API (operator module, range, sum).
class I(int):
    pass
class J(int):
    def __add__(self, o): return "J.add"
    def __radd__(self, o): return "J.radd"
class R:
    def __radd__(self, o): return ("R.radd", o)
    def __rfloordiv__(self, o): return "R.rfloordiv"

vals = [3, -7, 2 ** 70, 2.5, True, I(5), J(4)]
ops = [operator.add, operator.sub, operator.mul, operator.floordiv, operator.truediv, operator.mod,
       operator.and_, operator.or_, operator.xor]
for f in ops:
    res = []
    for a in vals:
        for b in vals:
            if isinstance(b, J) and type(a) is int and a > 2 ** 63:
                continue
            try:
                r = f(a, b)
                res.append((type(r).__name__, round(r, 6) if isinstance(r, float) else r))
            except TypeError as e:
                res.append(str(e))
    print(f.__name__, res)
print(operator.add(1, R()), operator.floordiv(7, R()), operator.sub(True, False), operator.and_(True, False))
for a, b in [(1, "x"), (1, None)]:
    try:
        operator.add(a, b)
    except TypeError as e:
        print(e)
print(list(range(3, 100, 7)), len(range(10 ** 20, 10 ** 20 + 50, 3)), sum([1, 2, 3, 2 ** 64]))

# User-defined operators at a hot site.
class V:
    def __init__(self, x): self.x = x
    def __sub__(self, o):
        if isinstance(o, V): return V(self.x - o.x)
        if o == "ni": return NotImplemented
        return V(self.x - o)
    def __mul__(self, o):
        return NotImplemented
    def __repr__(self): return "V(%r)" % self.x
class W:
    def __rsub__(self, o): return "W.rsub"
    def __rmul__(self, o): return "W.rmul"
class Sub(V):
    def __rsub__(self, o): return "Sub.rsub"
class U:
    pass

def sub(a, b):
    return a - b
def mul(a, b):
    return a * b

out = []
for i in range(3000):
    out.append(sub(V(i), V(1)).x + sub(V(i), 2).x)
    if i % 300 == 0:
        out.append(sub(V(1), W()))
        out.append(mul(V(1), W()))
        out.append(sub(V(1), Sub(2)))
        for a, b in [(V(1), "ni"), (U(), U()), (V(1), V(2)), (U(), V(1))]:
            try:
                out.append(mul(a, b) if isinstance(b, V) and isinstance(a, V) else sub(a, b))
            except TypeError as e:
                out.append(str(e))
print(len(out), sum(o for o in out if isinstance(o, int)), [o for o in out if not isinstance(o, int)])

# Reassigning a class's operator method must reach compiled call sites.
class P:
    def __sub__(self, o): return 1
    def m(self): return 1
def explicit(a):
    return a.__sub__(None)
def implicit(a):
    return a - None
seen = []
for i in range(3000):
    seen.append((explicit(P()), implicit(P()), P().m()))
    if i % 100 == 99:
        old_sub, old_m = P.__sub__, P.m
        P.__sub__ = lambda s, o: 2
        P.m = lambda s: 2
        seen.append((explicit(P()), implicit(P()), P().m(), P.__sub__(P(), None)))
        P.__sub__, P.m = old_sub, old_m
print(len(seen), sum(1 for s in seen if s[0] == 2), sum(1 for s in seen if s[1] == 2), sum(1 for s in seen if s[2] == 2))
