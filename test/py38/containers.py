l = [3, 1, 2]
l.append(5)
l.extend([0])
l.sort()
print(l, l[::-1], l[1:3], l.index(2), l.count(1), l.pop(), l, sorted(l, reverse=True), sorted(["b", "A", "c"], key=str.lower))
l[1:3] = ["x"]
del l[0]
print(l, [1, 2] + [3], [0] * 3, [1, 2] == [1, 2], [1, 2] < [1, 3], list(range(3, 10, 3)), list(reversed([1, 2])))
t = (1, "a", None)
print(t, t[1:], t + (2,), len(t), (1,) * 3, tuple("ab"), t.index("a"), (1, 2) < (1, 3), hash((1, 2)) == hash((1, 2)))
a, *rest = [1, 2, 3] if False else (1, [2, 3])
print(a, rest)
d = {"b": 1, "a": 2}
d["c"] = 3
print(sorted(d.items()), d.get("z", 0), d.setdefault("a", 9), d.pop("b"), "c" in d, len(d), sorted(d), sorted(d.values()))
print(type(d.keys()).__name__, sorted(d.keys() & {"a", "q"}), {k: v * 2 for k, v in sorted(d.items())}, dict(a=1), dict([(1, 2)]))
print(dict.fromkeys("ab", 0) == {"a": 0, "b": 0}, {**{}} if False else {}, list({1: 2}.items()))
s = {1, 2, 3}
print(s & {2, 3, 4}, s | {9}, s - {1}, s ^ {3, 4}, {x % 3 for x in range(10)}, frozenset([1, 1]), 2 in s, set() == set())
print(len(range(10**18)), range(10)[3], range(0, 10, 2)[-1], list(range(5)[1:3]), 3 in range(5), range(5).index(3))
print(list(zip("ab", [1, 2])), list(enumerate("ab", 1)), list(map(lambda x: x * 2, [1, 2])), list(filter(None, [0, 1, 2])))
print(any([0, 1]), all([]), max("abc"), min([], default=None), sorted({3: 1, 1: 2}), list(iter([1, 2])), next(iter("x")))
it = iter([1, 2])
print(next(it), next(it), next(it, "done"))
m = memoryview(b"abcd")
print(m[1], bytes(m[1:3]), len(m), m.tobytes())
print(slice(1, 2), slice(None, 5, 2).indices(10), [1, 2, 3, 4][slice(1, None)], ...)
