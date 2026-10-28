if (n := 10) > 5:
    print("n", n)
data = [1, 2, 3, 4]
print([y for x in data if (y := x * 2) > 4], y)
def f():
    total = 0
    vals = [total := total + v for v in range(5)]
    return vals, total
print(f())
def g(lines):
    out = []
    it = iter(lines)
    while (line := next(it, None)) is not None:
        out.append(line)
    return out, line
print(g(["a", "b"]))
def h():
    any((hit := v) > 2 for v in [1, 3, 5])
    return hit
print(h())
print([[(z := i + j) for j in range(2)] for i in range(2)], z)
print((lambda: (w := 5) + w)())
def k():
    def inner():
        return [(q := i) for i in range(3)]
    r = inner()
    return r
print(k())
try:
    exec("class C:\n    [(c := 1) for i in range(1)]")
except SyntaxError as e:
    print("SyntaxError", e.msg)
