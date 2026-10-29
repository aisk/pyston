# Reflected operator priority for int subclasses, zero division errors and float subclasses.

class I(int):
    def __add__(self, o): return ("I.add", int(o) % 7)
    def __radd__(self, o): return ("I.radd", int(o) % 7)
    def __sub__(self, o): return "I.sub"
    def __rsub__(self, o): return "I.rsub"
    def __mul__(self, o): return "I.mul"
    def __rmul__(self, o): return "I.rmul"
    def __truediv__(self, o): return "I.div"
    def __rtruediv__(self, o): return "I.rdiv"
    def __pow__(self, o, m=None): return "I.pow"
    def __rpow__(self, o): return "I.rpow"
    def __lt__(self, o): return "I.lt"
    def __gt__(self, o): return "I.gt"

def ops(x, i):
    return (x + i, i + x, x - i, i - x, x * i, i * x, x / i, i / x, x ** i, x < i, i < x)

big = 2 ** 100
for x in (big, -big, 5, True, 3.5):
    print(set(repr(ops(x, I(3))) for k in range(300)))
y = big
y += I(3)
print(y)

class F(float):
    pass

class J(int):
    pass

vals = [1, 0, big, -big, True, False, 1.5, 0.0, -0.0, J(2), J(0), F(0.0)]
exprs = ["a / b", "a // b", "a % b", "divmod(a, b)"]
for a in vals:
    for b in vals:
        out = []
        for e in exprs:
            try:
                r = eval(e)
                out.append(repr(r))
            except Exception as ex:
                out.append("%s: %s" % (type(ex).__name__, ex))
        print(repr(a), repr(b), out)

def zero_div(a, b):
    msgs = set()
    for k in range(3000):
        for f in (lambda: a / b, lambda: a // b, lambda: a % b, lambda: divmod(a, b)):
            try:
                f()
            except ZeroDivisionError as e:
                msgs.add(str(e))
    return sorted(msgs)
for a, b in [(1, 0), (1.5, 0), (1, 0.0), (1.5, 0.0), (big, 0), (big, 0.0)]:
    print(zero_div(a, b))

for e in ["pow(F(0.0), 1, 7)", "pow(1.5, 2, 7)", "F(2.0).__pow__(2)", "F(2.0) ** F(0.5)", "float.__pow__(1, 2)",
          "F(0.0) ** -1"]:
    try:
        print(e, eval(e))
    except Exception as ex:
        print(e, type(ex).__name__, ex)
