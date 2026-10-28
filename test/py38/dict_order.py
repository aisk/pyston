d = {}
for k in ['z', 'a', 'm', 3, 1.5, (1, 2), 'b']:
    d[k] = str(k)
print(d, list(d), list(d.values()), list(d.items()))
del d['a']; d['a'] = 'again'; d['z'] = 'changed'
print(d)
print(d.popitem(), d)
e = {i: i for i in range(100)}
for i in range(0, 100, 3):
    del e[i]
for i in range(200, 220):
    e[i] = i
print(list(e)[:10], list(e)[-5:], len(e))
print(e.copy() == e, list(e.copy())[:5])
def f(**kw): return list(kw)
print(f(b=1, a=2, c=3), dict(x=1, y=2, a=3), dict([(3, 'c'), (1, 'a')]))
class C:
    z = 1
    a = 2
    def m(self): pass
print([k for k in C.__dict__ if not k.startswith('__')])
s = {**{'q': 1, 'p': 2}, 'o': 3}
print(s, {k: v for k, v in zip('cab', range(3))})
import collections, enum
print({'b': 1, 'a': [1, {'d': 2, 'c': 3}]})
class Color(enum.Enum):
    RED = 1
    GREEN = 2
    BLUE = 3
print(list(Color), collections.OrderedDict(b=1, a=2), collections.Counter('abracadabra').most_common(3))
x = dict.fromkeys('hello')
print(x, sorted(x) == sorted(set('hello')))
it = iter(d)
d2 = dict(d)
d2.update(k=1)
print(d2, d2.setdefault('new', 5), d2)
print(repr({1: {2: {}}}), {}.popitem if False else 'ok')
