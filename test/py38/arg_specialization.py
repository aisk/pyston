# Functions get a version specialized on int and float arguments, and one that takes anything.

def add(a, b):
    return a + b

def mixed(a, b=2.5, *args, c=1, **kw):
    return (a * b + c, len(args), sorted(kw))

def fib(n):
    return n if n < 2 else fib(n - 1) + fib(n - 2)

def overflow(a, b):
    return a * b + a

class I(int):
    def __add__(self, o):
        return "I.add"

out = []
for i in range(3000):
    out.append(add(i, 1))
for x in [(1.5, 2), (1, 2.5), ("a", "b"), (I(1), 2), (2 ** 70, 1), (True, 1), ([1], [2]), (3, 4)]:
    out.append([add(*x) for i in range(300)][-1])
print(out[-12:], sum(o for o in out[:3000]))

res = []
for i in range(3000):
    res.append(mixed(i))
res.append(mixed(2, 3))
res.append(mixed(1.5, 2, 3, 4, c=2, z=1))
res.append(mixed("x", 3, c="!"))
print(res[-4:])

print([fib(n) for n in range(20)], fib(22.0))

print([overflow(2 ** 31 + i, 2 ** 31) for i in range(3000)][-1], overflow(2 ** 62, 4), overflow(1.5, 2))
