# range iteration, including the iterator protocol Pyston adds to range_iterator.
def total(n, step=1):
    t = 0
    for i in range(0, n, step):
        t += i
    return t
print([total(n) for n in (0, 1, 10, 1000)], total(100, 7), total(-5), total(10, -1))
it = iter(range(3))
print(type(it).__name__, it.__next__(), next(it), list(it), list(it))
try:
    it.__next__()
except StopIteration:
    print("StopIteration")
print(list(range(5, 0, -2)), list(reversed(range(4))), list(reversed(range(1, 10, 3))))
it = iter(range(10))
next(it)
print(it.__length_hint__(), it.__reduce__()[1:])
it.__setstate__(8)
print(list(it))
big = 2 ** 64
print(type(iter(range(big, big + 2))).__name__, list(range(big, big + 2)), list(range(-big, -big + 3, 2)))
print(list(range(2 ** 63 - 2, 2 ** 63)), list(reversed(range(-2 ** 63, -2 ** 63 + 2))))
print(sum(i for i in range(100)), [i * j for i in range(3) for j in range(2)])
