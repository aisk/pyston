import collections.abc
import os, sys, collections, itertools, re, math, string, struct, enum, operator, copy, heapq, bisect, io
print(os.path.join("a", "b"), os.path.basename("/x/y.py"), os.sep, isinstance(os.environ, collections.abc.Mapping))
print(sys.version_info[:2], sys.implementation.name in ("cpython", "pyston"), sys.maxsize, sys.byteorder, type(sys.flags).__name__)
print(collections.Counter("hello").most_common(1), collections.defaultdict(int)["x"], collections.deque([1, 2], maxlen=2))
P = collections.namedtuple("P", "x y")
print(P(1, 2), P(1, 2).x, P._fields, P(1, 2)._replace(y=5))
print(list(itertools.chain([1], [2])), list(itertools.product("ab", repeat=2))[:3], list(itertools.islice(itertools.count(), 3)))
print(re.findall(r"\d+", "a1b22c333"), re.sub(r"(\w)(\d)", r"\2\1", "a1b2"), re.match(r"(?P<w>\w+)", "hi there").group("w"))
print(math.sqrt(16), math.pi, math.floor(-1.5), math.gcd(12, 18), math.factorial(20), math.isclose(0.1 + 0.2, 0.3))
print(string.ascii_lowercase[:5], struct.pack("<ih", 1, 2), struct.unpack("<ih", b"\x01\x00\x00\x00\x02\x00"))
class Color(enum.Enum):
    RED = 1
    GREEN = 2
print(Color.RED, Color(2), Color.RED.name, Color["GREEN"].value)
print(operator.add(1, 2), operator.itemgetter(1)("ab"), copy.deepcopy([[1], [2]]), heapq.nsmallest(2, [5, 1, 3]), bisect.bisect([1, 3], 2))
buf = io.StringIO()
print("to buffer", file=buf)
print(repr(buf.getvalue()), io.BytesIO(b"xy").read())
