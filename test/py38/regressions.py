# Bugs found in review of the first 3.8 round.
import copy, sys, threading

print(10 ** 18 / 3, 3 / 10 ** 20, type(2 ** 3) is int, type(10 ** 20 - 10 ** 20 + 5) is int)
print('%d' % 2 ** 70, format(2 ** 70, ','), int.__repr__(2 ** 70), copy.copy(8), copy.copy(10 ** 20))

c = threading.Condition()
with c:
    print(c.wait(0.01))

def p(x):
    print("eval", x)
    return x
d = {p("k"): p("v")}
def f(*a, **k): return a, sorted(k.items())
print(f(p(1), *[p(2)], p(3), k=p(4)))
print(f(p(1), *p([2]), k=p(3), **p({"z": 4})))

Ellipsis = 5
print(...)
del Ellipsis

try:
    {**[(1, 2)]}
except TypeError as e:
    print(e)

def closure_exc():
    try:
        raise KeyError
    except KeyError as e:
        g = lambda: e
    return g
closure_exc()

class H:
    def __eq__(s, o): return True
try:
    hash(H())
except TypeError as e:
    print(e)

class B:
    def __bool__(s): return 1
try:
    bool(B())
except TypeError as e:
    print(e)
class L:
    def __len__(s): return -1
try:
    bool(L())
except ValueError as e:
    print(e)

class R:
    def m(self): pass
print(repr(R.m).split(" at")[0], len("\ud800"), ascii(chr(0xd800) + "x"))

class K: pass
r = sys.getrefcount(K)
for i in range(100):
    K()
print(sys.getrefcount(K) - r < 5)

# Big ints look like plain ints, and int's methods accept them.
b = 2**70
print(type(b) is int, b.__class__ is int, type(b).__name__)
print(int.__add__(b, 1), int.__add__(1, b), int.__rsub__(b, 3), int.__index__(b))
print(int.__eq__(b, b), int.__lt__(3, b), (3).__lt__(b), int.__rrshift__(b, 3))
print(copy.deepcopy([b, -b]), [(b * 7).__reduce_ex__(p)[1][-1] == b * 7 for p in range(2, 6)])

# Comparisons cached by the rewriter must still handle NotImplemented: object.__eq__ (which
# classes inherit) returns it for different objects.
class CA: pass
class CB: pass
def same(a, b):
    return a == b
for i in range(300):
    same(CA, CA)
print(same(CA, CB), same(CA, CA), same(object(), object()))

# Errors instead of crashes when containers change during comparisons.
class Clear:
    def __hash__(self): return 0
    def __eq__(self, o):
        other.clear()
        return False
other = set()
other = {Clear() for i in range(10)}
s = {0}
s.update(other)
d = {}
for i in range(3):
    d[i] = i
try:
    for k in d:
        d[k + 10] = 0
except RuntimeError as e:
    print(e)
print(pow(3, -1, 7), pow(38, -1, 97))

# != only tries the reflected __ne__, not the reflected __eq__.
ne_calls = []
class NeLeft:
    def __eq__(*args):
        ne_calls.append('Left.__eq__')
        return NotImplemented
class NeRight:
    def __eq__(*args):
        ne_calls.append('Right.__eq__')
        return NotImplemented
    def __ne__(*args):
        ne_calls.append('Right.__ne__')
        return NotImplemented
NeLeft() != NeRight()
print(ne_calls)

# Catching something that is not an exception class.
try:
    try:
        raise ValueError
    except "x":
        pass
except TypeError as e:
    print(e)

# copy with __getnewargs_ex__.
import copy
class KwOnly(int):
    def __new__(cls, *, foo):
        self = int.__new__(cls)
        self.foo = foo
        return self
    def __getnewargs_ex__(self):
        return (), {'foo': self.foo}
print(copy.copy(KwOnly(foo=42)).foo)

for f in [lambda: 1j // 1, lambda: 1j % 1, lambda: int.__new__(bool, 0)]:
    try:
        f()
    except TypeError as e:
        print(e)
