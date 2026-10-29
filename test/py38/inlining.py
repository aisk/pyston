# Calls to small global functions get inlined in the LLVM tier.  These check that the frames, tracebacks and deopts
# of the inlined functions behave like normal calls.
import sys
import traceback

def add(a, b):
    return a + b

def loop_add(n):
    s = 0
    for i in range(n):
        s = add(s, i)
    return s

print([loop_add(3000) for k in range(5)])

# Different argument and result types.
def pick(a, b):
    if a:
        return b
    return None

def mixed(n):
    out = []
    for i in range(n):
        out.append(pick(i % 3, i * 1.5))
        out.append(add("x", str(i % 2)))
    return out[-6:]

print(mixed(3000))

def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

print([fib(n) for n in range(22)])

# A callee with a loop, and one that handles its own exceptions.
def total(seq):
    t = 0
    for x in seq:
        t += x
    return t

def safe_div(a, b):
    try:
        return a // b
    except ZeroDivisionError:
        return -1

def loops(n):
    s = 0
    for i in range(n):
        s += total((i, 1, 2)) + safe_div(i, i % 4)
    return s

print(loops(3000))

# Exceptions that leave the inlined function.
def boom(x):
    if x > 2995:
        raise ValueError("boom %d" % x)
    return x

def catch(n):
    caught = []
    for i in range(n):
        try:
            boom(i)
        except ValueError as e:
            caught.append((str(e), [f.name for f in traceback.extract_tb(e.__traceback__)]))
    return caught

print(catch(3000))

def no_catch(n):
    s = 0
    for i in range(n):
        s += boom(i)
    return s

try:
    no_catch(3000)
except ValueError as e:
    print(e, [(f.name, f.line) for f in traceback.extract_tb(e.__traceback__)])

def get_item(d, k):
    return d[k]

def lookups(n):
    d = {i: i for i in range(10)}
    s = 0
    for i in range(n):
        try:
            s += get_item(d, i % 12)
        except KeyError as e:
            s += 1000
    return s

print(lookups(3000))

# Frame introspection from inside the inlined function.
def frames(x):
    f = sys._getframe()
    return (f.f_code.co_name, f.f_back.f_code.co_name, f.f_back.f_locals["i"], sorted(locals()), f.f_lineno)

def introspect(n):
    out = None
    for i in range(n):
        y = i * 2
        out = frames(i)
    return out, y

print(introspect(3000))

# Deopts in the inlined function: its type speculation fails, or its int arithmetic overflows.
def double(x):
    return x + x

def speculation(n, v):
    out = []
    for i in range(n):
        out.append(double(i))
    out.append(double(v))
    out.append(double(i))
    return out[-3:]

print(speculation(3000, "ab"), speculation(3000, 2 ** 62), speculation(3000, [1]))

def square(x):
    return x * x

def overflow(n):
    s = 0
    for i in range(n):
        s += square(i + 2 ** 31)
    return s

print(overflow(3000))

# The global changes, or the code of the function does.
def one(x):
    return 1

def two(x):
    return 2

def callg(n):
    s = 0
    for i in range(n):
        s += g(i)
        if i == 2990:
            set_g(two)
    return s

def set_g(f):
    global g

    g = f

g = one
print(callg(3000), callg(3000))
g = one
for k in range(3):
    print(callg(3000))

def three(x):
    return 3

def callone(n):
    s = 0
    for i in range(n):
        s += one(i)
        if i == 2990:
            one.__code__ = three.__code__
    return s

print(callone(3000))

# Inlining into a generator.
def gen(n):
    for i in range(n):
        yield add(i, 1)

print(sum(gen(3000)))

# A deopt in the inlined function, after which the interpreter raises.  The traceback keeps every frame.
def sq_check(x):
    y = x * x
    if y > 10 ** 20:
        raise ValueError(y)
    return y + 1

def deopt_raise(n):
    s = 0
    for i in range(n):
        x = i if i < n - 1 else 2 ** 40
        s += sq_check(x)
    return s

try:
    deopt_raise(3000)
except ValueError as e:
    print(e, [(f.name, f.line) for f in traceback.extract_tb(e.__traceback__)])

# The inlined function looks at the locals of its caller, also after it deopts.
def peek_caller(a):
    b = a * a
    f = sys._getframe(1)
    return sorted((k, v) for k, v in f.f_locals.items() if k in ("s", "t", "i")), b

def peeked(n, big):
    s = 0
    t = 1.5
    for i in range(n):
        s += i
        t *= 1.0001
        r = peek_caller(i)
    return r, peek_caller(big)

print(peeked(3000, 2 ** 40))
