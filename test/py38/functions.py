def f(a, b=2, *args, c, d=4, **kw):
    return a, b, args, c, d, sorted(kw.items())
print(f(1, c=3), f(1, 2, 3, 4, c=5, e=6), f(*[1, 2], **{"c": 3, "d": 5}))
def g(x, *, y=1):
    return x + y
print(g(1), g(1, y=2), g.__defaults__, g.__kwdefaults__, g.__code__.co_argcount, g.__code__.co_kwonlyargcount)
for call in [lambda: g(1, 2), lambda: f(1), lambda: g(y=1)]:
    try:
        call()
    except TypeError as e:
        print("TypeError")
def counter():
    n = 0
    def inc():
        nonlocal_n = n + 1
        return nonlocal_n
    return inc
print(counter()())
def deco(fn):
    def wrapper(*a, **k):
        return ("wrapped", fn(*a, **k))
    return wrapper
@deco
def h(x, *, k=0):
    return x + k
print(h(1, k=2))
print((lambda *a, k=5: (a, k))(1, 2), (lambda: 1).__name__, h.__name__, f.__name__, f.__qualname__)
def gen(n):
    for i in range(n):
        x = yield i
        if x:
            print("sent", x)
    return "end"
g2 = gen(3)
print(next(g2), g2.send("hi"), list(g2))
def outer():
    r = yield from gen(2)
    print("returned", r)
    yield from range(2)
print(list(outer()))
print(list(x * y for x in range(3) for y in range(2) if x), sum(i for i in range(5)))
def raises_stop():
    yield 1
    raise StopIteration
try:
    print(list(raises_stop()))
except RuntimeError as e:
    print("RuntimeError", e)
import functools
print(functools.reduce(lambda a, b: a * b, range(1, 6)), functools.partial(g, y=10)(1))
@functools.lru_cache(maxsize=None)
def fib(n):
    return n if n < 2 else fib(n - 1) + fib(n - 2)
print(fib(50))

def posonly(a, b, /, c, *, d=4, **kw):
    return a, b, c, d, sorted(kw.items())
print(posonly(1, 2, 3), posonly(1, 2, c=3, a=9))
try:
    posonly(a=1, b=2, c=3)
except TypeError:
    print("TypeError")
print(posonly.__code__.co_posonlyargcount)
