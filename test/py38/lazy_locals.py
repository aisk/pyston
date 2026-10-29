# Frame introspection of hot functions, whose int and float locals the LLVM tier keeps out of the frame.
import sys
import traceback

def peek():
    f = sys._getframe(1).f_locals
    return f.get("i"), f.get("s"), f.get("x")

def from_callee(n):
    s = 0
    x = 0.5
    r = None
    for i in range(n):
        s += i
        x *= 1.0
        if i == n - 1:
            r = peek()
    return r

def with_locals(n):
    s = 0
    for i in range(n):
        s += i * 2
    d = locals()
    return sorted((k, v) for k, v in d.items() if k != "d")

def fails(n):
    s = 0
    for i in range(n):
        s += i
        if i == n - 1:
            s / 0

def tb_locals(n):
    try:
        fails(n)
    except ZeroDivisionError as e:
        tb = e.__traceback__
        while tb.tb_next:
            tb = tb.tb_next
        return tb.tb_frame.f_locals["s"], tb.tb_frame.f_locals["i"], tb.tb_lineno

def caught(n):
    s = 0
    for i in range(n):
        s += i
        try:
            if i == n - 1:
                raise KeyError(i)
        except KeyError:
            return sys._getframe().f_locals["s"], i

def escaped_frame(n):
    s = 0
    x = 1.5
    for i in range(n):
        s += i
        x += 0.5
    return sys._getframe()

def maybe_undefined(n):
    out = []
    for i in range(n):
        if i % 2:
            y = i * 3
        if i == n - 1:
            out.append(sorted(locals().items()))
        if i % 2:
            del y
    return out

def cell(n):
    s = 0
    for i in range(n):
        s += i
    return (lambda: s)(), sorted(locals().items())[-1]

def formatted(n):
    t = 0
    for i in range(n):
        t += i
        if i == n - 1:
            stack = traceback.extract_stack()
    return t, stack[-1].name, "%(t)d %(i)d" % locals()

def index_error(n):
    a = list(range(n - 1))
    s = 0
    for i in range(n):
        s += a[i]
    return s

def tb_index_error(n):
    try:
        index_error(n)
    except IndexError as e:
        tb = e.__traceback__
        lines = []
        while tb:
            lines.append(tb.tb_frame.f_code.co_name)
            last = tb
            tb = tb.tb_next
        return lines, last.tb_frame.f_locals["s"], last.tb_frame.f_locals["i"]

def caught_index_error(n):
    a = list(range(n - 1))
    s = 0
    for i in range(n):
        if i % 2:
            y = i * 2
        try:
            s += a[i]
        except IndexError:
            return sorted(sys._getframe().f_locals.items())[1:]
        if i % 2:
            del y

for n in [10, 3000, 20000]:
    f = escaped_frame(n)
    print(from_callee(n), with_locals(n), tb_locals(n), caught(n), f.f_locals["s"], f.f_locals["x"],
          maybe_undefined(n), cell(n), formatted(n), tb_index_error(n),
          caught_index_error(n))
