class Base:
    registry = []
    def __init_subclass__(cls, tag=None, **kw):
        super().__init_subclass__(**kw)
        Base.registry.append((cls.__name__, tag))
    def __init__(self, x):
        self.x = x
    def describe(self):
        return "Base(%s)" % self.x
    def __repr__(self):
        return "<%s %r>" % (type(self).__name__, self.x)
class Child(Base, tag="child"):
    def __init__(self, x, y):
        super().__init__(x)
        self.y = y
    def describe(self):
        return "Child:" + super().describe()
    @property
    def total(self):
        return self.x + self.y
    @staticmethod
    def s():
        return "static"
    @classmethod
    def c(cls):
        return cls.__name__
print(Base.registry, Child(1, 2).describe(), Child(1, 2).total, Child.s(), Child.c(), repr(Child(3, 4)))
print(Child.__qualname__, Child.__mro__, Child.__bases__, isinstance(Child(1, 1), Base), issubclass(Child, object))
class Field:
    def __set_name__(self, owner, name):
        self.name = name
    def __get__(self, obj, objtype=None):
        return self if obj is None else obj.__dict__.get(self.name, "unset")
class Rec:
    a = Field()
print(Rec.a.name, Rec().a)
class Meta(type):
    def __new__(mcls, name, bases, ns, **kw):
        ns["made_by"] = "Meta"
        return super().__new__(mcls, name, bases, ns)
    @classmethod
    def __prepare__(mcls, name, bases, **kw):
        return {"prepared": True}
class WithMeta(metaclass=Meta):
    pass
print(type(WithMeta).__name__, WithMeta.made_by, WithMeta.prepared)
class Vec:
    def __init__(self, *c):
        self.c = c
    def __add__(self, o):
        return Vec(*[a + b for a, b in zip(self.c, o.c)])
    def __eq__(self, o):
        return self.c == o.c
    def __hash__(self):
        return hash(self.c)
    def __len__(self):
        return len(self.c)
    def __getitem__(self, i):
        return self.c[i]
    def __bool__(self):
        return any(self.c)
    def __lt__(self, o):
        return self.c < o.c
print((Vec(1, 2) + Vec(3, 4)).c, Vec(1) == Vec(1), len(Vec(1, 2, 3)), list(Vec(5, 6)), bool(Vec(0)), sorted([Vec(2), Vec(1)])[0].c)
class Slotted:
    __slots__ = ("a",)
s = Slotted()
s.a = 1
try:
    s.b = 2
except AttributeError:
    print("AttributeError", s.a)
import abc
class Shape(abc.ABC):
    @abc.abstractmethod
    def area(self): ...
class Sq(Shape):
    def area(self):
        return 4
try:
    Shape()
except TypeError as e:
    print("TypeError", e)
print(Sq().area(), isinstance(Sq(), Shape))
print(type(type), type(object), object.__class__, type.__mro__, type("Dyn", (Base,), {"z": 1}).z, getattr(Child, "missing", "dflt"))

# Class bodies run in the namespace returned by __prepare__.
class LoggingNs(dict):
    def __setitem__(self, k, v):
        if not k.startswith('__'):
            print("set", k)
        super().__setitem__(k, v)
class PrepMeta(type):
    @classmethod
    def __prepare__(mcls, name, bases, **kw):
        print("prepare", name, kw)
        return LoggingNs()
    def __new__(mcls, name, bases, ns, **kw):
        return super().__new__(mcls, name, bases, dict(ns))
    def __init__(cls, name, bases, ns, **kw):
        super().__init__(name, bases, ns)
class Prepared(metaclass=PrepMeta, flag=1):
    x = 1
    x = 2
    y: int
    del x
print(sorted(k for k in Prepared.__dict__ if not k.startswith('__')), Prepared.__annotations__)
import enum
try:
    class Dup(enum.Enum):
        A = 1
        A = 2
except TypeError as e:
    print("TypeError", e)
ns = {'__annotations__': {'kept': 1}}
exec("z: int = 3", ns)
print(ns['__annotations__'])
