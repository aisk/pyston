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

# Method calls get inlined too, guarded on the class of the object, the class not changing, the code of the method
# and the object not having an attribute with the name.
class P:
    def __init__(self, x):
        self.x = x

    def get(self):
        return self.x

    def add(self, y):
        return self.x + y

class Q(P):
    def get(self):
        return -self.x

def methods(objs, n):
    s = 0
    for i in range(n):
        p = objs[i * len(objs) // n]
        s += p.add(i) + p.get()
    return s

p = P(3)
print(methods([p], 3000), methods([p, Q(4)], 3000), methods([p, P(2.5)], 3000))
shadowed = P(5)
shadowed.get = lambda: 100
print(methods([p, shadowed], 3000))

def replace_method(n):
    s = 0
    p = P(1)
    for i in range(n):
        s += p.get()
        if i == 2990:
            P.get = lambda self: 10
        if i == 2995:
            P.get = Q.get
    return s

print(replace_method(3000))

def change_code(n):
    s = 0
    p = P(1)
    for i in range(n):
        s += p.add(i)
        if i == 2990:
            P.add.__code__ = (lambda self, y: 1000).__code__
    return s

print(change_code(3000))

class R:
    def __init__(self):
        self.v = 1

    def value(self):
        return self.v

def getattribute_added(n):
    s = 0
    r = R()
    for i in range(n):
        s += r.value()
        if i == 2990:
            R.__getattribute__ = lambda self, name: (lambda: 7) if name == "value" else object.__getattribute__(self, name)
    return s

print(getattribute_added(3000))

# The result of the call that isn't inlined has another type than the inlined result.
class Num:
    def __init__(self, v):
        self.v = v

    def twice(self):
        return self.v * 2

def result_types(objs, n):
    out = []
    for i in range(n):
        out.append(objs[i * len(objs) // n].twice())
    return out[-3:], len(set(map(type, out)))

print(result_types([Num(2)], 3000), result_types([Num(2), Num("ab"), Num(1.5)], 3000))

class Boom:
    def check(self, x):
        if x > 2995:
            raise ValueError("method %d" % x)
        return x

def method_raise(n):
    b = Boom()
    s = 0
    for i in range(n):
        s += b.check(i)
    return s

try:
    method_raise(3000)
except ValueError as e:
    print(e, [(f.name, f.line) for f in traceback.extract_tb(e.__traceback__)])

# Class calls inline __init__ when the class has object.__new__.
class V:
    def __init__(self, x, y):
        self.x = x
        self.y = y

    def __add__(self, o):
        return V(self.x + o.x, self.y + o.y)

    def __mul__(self, k):
        return V(self.x * k, self.y * k)

    def __repr__(self):
        return "V(%r, %r)" % (self.x, self.y)

def make(cls, n):
    out = None
    for i in range(n):
        out = cls(i, i + 1)
    return out

print(make(V, 3000), type(make(V, 3000)).__name__)

class BadInit:
    def __init__(self):
        if self.flag:
            return 5

def bad_init(n):
    BadInit.flag = False
    out = []
    for i in range(n):
        if i == 2990:
            BadInit.flag = True
        try:
            out.append(type(BadInit()).__name__)
        except TypeError as e:
            out.append(str(e))
    return out[-1], len(out)

print(bad_init(3000))

class W:
    def __init__(self, x):
        self.x = x

def ctor_changes(n):
    out = []
    for i in range(n):
        w = W(i)
        out.append(w if type(w) is int else w.x)
        if i == 2990:
            W.__init__ = lambda self, x: setattr(self, "x", -x)
        if i == 2994:
            W.__new__ = lambda cls, x: 7
        if i == 2997:
            W.__new__ = lambda cls, x: object.__new__(cls)
            W.__init__ = (lambda self, x: setattr(self, "x", x * 10))
    return out[-12:]

print(ctor_changes(3000))

class U:
    def __init__(self, a, b):
        self.a = a

def ctor_rebind(n):
    global U
    out = []
    old = U
    for i in range(n):
        out.append(type(U(i, i)).__name__)
        if i == 2990:
            U = V
    U = old
    return out[-3:]

print(ctor_rebind(3000))

class Abstract:
    def __init__(self):
        pass

def abstract(n):
    out = []
    for i in range(n):
        if i == 2995:
            Abstract.__abstractmethods__ = frozenset(["f"])
        try:
            out.append(type(Abstract()).__name__)
        except TypeError as e:
            out.append(str(e))
    return out[-6:]

print(abstract(3000))

class CtorRaise:
    def __init__(self, x):
        if x > 2995:
            raise ValueError("ctor %d" % x)

def ctor_raise(n):
    for i in range(n):
        CtorRaise(i)

try:
    ctor_raise(3000)
except ValueError as e:
    print(e, [(f.name, f.line) for f in traceback.extract_tb(e.__traceback__)])

# Operator methods of user classes get inlined too.
def vec_sum(n):
    s = V(0, 0)
    for i in range(n):
        s = s + V(i, 1) * 2
        s += V(1, 1)
    return s

print(vec_sum(3000))

class A:
    def __init__(self, v):
        self.v = v

    def __add__(self, o):
        if isinstance(o, A):
            return A(self.v + o.v)
        return NotImplemented

    def __sub__(self, o):
        return self.v - o

    def __repr__(self):
        return "A(%r)" % self.v

class B:
    def __radd__(self, o):
        return "B.radd"

class SubA(A):
    def __radd__(self, o):
        return "SubA.radd"

class IA(A):
    def __iadd__(self, o):
        self.v += 100
        return self

def operands(n, others):
    out = []
    for i in range(n):
        a = A(i)
        o = others[i * len(others) // n]
        try:
            out.append(a + o)
        except TypeError as e:
            out.append(str(e))
    return out[-1]

print(operands(3000, [A(1)]), operands(3000, [A(1), B()]), operands(3000, [A(1), SubA(2)]),
      operands(3000, [A(1), 5]))

def inplace(n, cls):
    out = []
    for i in range(n):
        a = cls(i)
        a += A(1)
        out.append(a)
    return out[-1]

print(inplace(3000, A), inplace(3000, IA))

def sub_types(n, others):
    out = []
    for i in range(n):
        out.append(A(i) - others[i * len(others) // n])
    return out[-3:], len(set(map(type, out)))

print(sub_types(3000, [1]), sub_types(3000, [1, 1.5, 2 ** 70]))

def replace_op(n):
    out = []
    for i in range(n):
        out.append(A(i) - 1)
        if i == 2990:
            A.__sub__ = lambda self, o: "replaced"
    return out[-12:]

print(replace_op(3000))

class OpRaise:
    def __mul__(self, o):
        if o > 2995:
            raise ValueError("op %d" % o)
        return o

def op_raise(n):
    s = 0
    x = OpRaise()
    for i in range(n):
        s += x * i
    return s

try:
    op_raise(3000)
except ValueError as e:
    print(e, [(f.name, f.line) for f in traceback.extract_tb(e.__traceback__)])
