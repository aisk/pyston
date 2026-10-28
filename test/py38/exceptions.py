class MyError(ValueError):
    pass
def boom(n):
    if n == 0:
        raise MyError("deep", n)
    boom(n - 1)
try:
    boom(3)
except ValueError as e:
    print(type(e).__name__, e.args, str(e), isinstance(e, Exception))
try:
    try:
        raise KeyError("k")
    finally:
        print("finally")
except LookupError as e:
    print("caught", repr(e))
def f():
    try:
        return "try"
    finally:
        print("cleanup")
print(f())
try:
    {}["missing"]
except KeyError as e:
    print("KeyError", e)
try:
    [][1]
except IndexError as e:
    print("IndexError", e)
try:
    None.foo
except AttributeError as e:
    print("AttributeError", e)
try:
    undefined_name
except NameError as e:
    print("NameError", e)
import sys
try:
    raise RuntimeError("x")
except RuntimeError:
    print(sys.exc_info()[0].__name__)
print(sys.exc_info())
with open(__file__) as fp:
    print(fp.readline().strip())
class CM:
    def __enter__(self):
        print("enter")
        return self
    def __exit__(self, *a):
        print("exit", a[0].__name__ if a[0] else None)
        return True
with CM() as c, CM():
    raise ValueError
print(issubclass(StopIteration, Exception), BaseException.__subclasses__ is not None, OSError is IOError, FileNotFoundError.__mro__[1].__name__)
try:
    open("/nonexistent/x")
except FileNotFoundError as e:
    print("FileNotFoundError", e.errno)

# Exception chaining and Python 3 exc_info scoping
import sys
from contextlib import suppress
try:
    try:
        1/0
    except Exception:
        raise KeyError("k")
except KeyError:
    pass
print(sys.exc_info())
try:
    raise ValueError
except ValueError as e:
    print(repr(e.__context__))
with suppress(KeyError):
    {}[1]
print(sys.exc_info())
try:
    try:
        1/0
    finally:
        raise OSError
except OSError as e:
    print(repr(e.__context__))
def f():
    for i in range(3):
        try:
            raise KeyError(i)
        except KeyError:
            continue
    print(sys.exc_info())
f()
def g():
    try:
        raise TypeError("foo")
    except:
        try:
            raise KeyError("caught")
        finally:
            raise
try: g()
except KeyError as e: print("ok", repr(e))
class CM2:
    def __enter__(self): pass
    def __exit__(self, t, v, tb):
        print(sys.exc_info()[0])
        xyzzy
try:
    with CM2():
        1/0
except NameError as e:
    print(repr(e.__context__))
def g():
    try:
        raise TypeError("foo")
    except:
        yield 1
        raise
it = g(); next(it)
try: next(it)
except TypeError as e: print("ok", e)
def g():
    try:
        raise TypeError("foo")
    except:
        print(1, sys.exc_info()[0])
        yield 1
        print(2, sys.exc_info()[0])
        yield 2
it = g(); next(it); next(it)
try:
    raise (IndexError, KeyError)
except TypeError as e:
    print(e)
def tb_func():
    try:
        1/0
    except ZeroDivisionError as e:
        return e
e = tb_func()
print(type(e.__traceback__).__name__, e.__traceback__.tb_frame.f_code.co_name)
try:
    raise e
except ZeroDivisionError as e2:
    print(e2.__traceback__.tb_next.tb_frame.f_code.co_name)
