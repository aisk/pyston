import functools
x: int = 5
y: "str"
print(__annotations__)
class C:
    a: int = 1
    b: list
    if True:
        c: float = 2.0
    def m(self, p: int, *args: str, k: bool = True, **kw: dict) -> "C":
        return self
    @property
    def v(self) -> int:
        return 1
    @v.setter
    def v(self, val: int) -> None:
        pass
    @staticmethod
    def s(q: int): return q
print(C.__annotations__, C.a, C.c)
print(C.m.__annotations__, C.v.fget.__annotations__, C.v.fset.__annotations__, C.s(3), C.s.__annotations__)
def deco(f):
    @functools.wraps(f)
    def w(*a): return f(*a) * 2
    return w
@deco
def g(a: int) -> int:
    return a
print(g(2), g.__annotations__, g.__name__)
def plain(): pass
print(plain.__annotations__)
plain.__annotations__ = {'z': 1}
print(plain.__annotations__)
def local():
    t: int = 3
    u: undefined_name
    return t
print(local())
class P:
    x: int
    y: int = 2
    z: "P"
print(list(P.__annotations__), P.y, 'x' in P.__dict__)
