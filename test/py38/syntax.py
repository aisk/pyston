import sys

class M:
    def __matmul__(self, o): return "mm"
    def __rmatmul__(self, o): return "rmm"
    def __imatmul__(self, o): return "imm"
m = M()
print(m @ 1, 1 @ m)
m @= 2
print(m)

d = {**{"a": 1}, "b": 2, **{"c": 3}}
print(sorted(d.items()))

def f(*a, **k): return a, sorted(k.items())
print(f(*[1], 2, *(3, 4), **{"x": 1}, y=2, **{"z": 3}))
print(f(1, *[2]), f(**{"a": 1}, b=2))
try:
    f(a=1, **{"a": 2})
except TypeError as e:
    print("TypeError")

def outer():
    a, b = 1, 2
    def inner(): return a + b
    class C:
        class D:
            def m(self): pass
    return inner, C
inner, C = outer()
print(inner.__qualname__, C.__qualname__, C.D.__qualname__, C.D.m.__qualname__, (lambda: 0).__qualname__)
print([c.cell_contents for c in inner.__closure__], outer.__closure__)

def handlers():
    try:
        raise KeyError(1)
    except KeyError as e:
        try:
            raise ValueError(2)
        except ValueError:
            print(sys.exc_info()[0].__name__)
        print(sys.exc_info()[0].__name__)
    print(sys.exc_info())
    try:
        e
    except NameError:
        print("e unbound")
handlers()

print(int("1_000"), int("0x_1f", 0), int(b"101", 2), int(bytearray(b"ff"), 16), int("-0b101", 0))
for bad in ["1__0", "_1", "1_", "0777", "", "0x"]:
    try:
        int(bad, 0)
    except ValueError as e:
        print(e)
print(hash(2 ** 63), hash(-1), hash(2 ** 61 - 1), hash(2.0 ** 70) == hash(2 ** 70))
print((-2) ** 0.5, len(range(10 ** 18)))
print(object() == object(), object() != object(), object.__eq__(1, 1))

# Comprehensions have their own scope.
x = 'outer'
print([x for x in range(3)], x)

# continue inside finally
def cont():
    out = []
    for i in range(3):
        try:
            out.append(i)
        finally:
            continue
    return out
print(cont())

# Scoping errors reported by the symbol table pass.
for src in ["[i := 0 for i in range(5)]", "[x for x in (y := [1, 2])]", "def f():\n    nonlocal q",
            "x = 1\nglobal x", "def f(a):\n    global a", "class C:\n    [(z := 1) for _ in range(1)]"]:
    try:
        exec(src)
        print("no error", repr(src))
    except SyntaxError as e:
        print("SyntaxError:", e.msg)

# Compile-time SyntaxWarnings, turned into errors here.
import warnings
for src in ["x is 1", "assert (x, 'msg')", "[(1, 2) (3, 4)]", "[1, 2] ['a']", "{1} [0]"]:
    with warnings.catch_warnings():
        warnings.simplefilter("error")
        try:
            compile(src, "<w>", "exec")
            print("no warning", repr(src))
        except SyntaxError as e:
            print("SyntaxError:", e.msg)

class Spam:
    __x: int
    def f(self, *, __kw: 1): pass
print(Spam.f.__annotations__, Spam.__annotations__)
def annotated_local():
    x: int
    try:
        print(x)
    except UnboundLocalError:
        print("UnboundLocalError")
annotated_local()
try:
    int(x=1)
except TypeError:
    print("TypeError")
print(int("ff", base=16), int("0", 0))
