def f():
    x = 1
    def g(): return x
    del x
    try:
        g()
    except NameError as e:
        print("NameError", e)
    x = 2
    return g()
print(f())
def h():
    y = 1
    def inner():
        nonlocal y
        del y
    inner()
    try:
        print(y)
    except NameError as e:
        print("NameError", e)
h()
def k():
    try:
        1/0
    except ZeroDivisionError as e:
        f = lambda: e
    try:
        f()
    except NameError as err:
        print("NameError", err)
k()
def u():
    def g(): return z
    try:
        del z
    except NameError as e:
        print("NameError", e)
    z = 3
u()
