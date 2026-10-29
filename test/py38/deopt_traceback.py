# An exception raised by the interpreter after a JIT frame deopted keeps the traceback entries of every frame.
import traceback

def sq_check(x):
    y = x * x
    if y > 10 ** 20:
        raise ValueError(y)
    return y + 1

def deopt_raise(n):
    s = 0
    for i in range(n):
        s += sq_check(i if i < n - 1 else 2 ** 40)
    return s

def deopt_catch(n, big):
    s = 0
    for i in range(n):
        s += sq_check(i)
    try:
        sq_check(big)
    except ValueError as e:
        print("caught", [(f.name, f.line) for f in traceback.extract_tb(e.__traceback__)])
    return s

try:
    deopt_raise(3000)
except ValueError as e:
    print(e, [(f.name, f.line) for f in traceback.extract_tb(e.__traceback__)])
print(deopt_catch(3000, 2 ** 40), deopt_catch(3000, 2 ** 40))
