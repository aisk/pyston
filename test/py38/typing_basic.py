# PEP 560 hooks and the typing module.
class A:
    def __class_getitem__(cls, item):
        return ('cg', item)
print(A[int])
try:
    object()[1]
except TypeError as e:
    print(e)

class B: pass
class M:
    def __mro_entries__(self, bases):
        return (B,)
class F(M()): pass
print(F.__mro__, type(F.__orig_bases__[0]).__name__)

import typing
from typing import List, Dict, Optional, NamedTuple, TypeVar, Generic, Callable, Union
T = TypeVar('T')
class Box(Generic[T]):
    def __init__(self, v: T):
        self.v = v
class P(NamedTuple):
    x: int
    y: str = 'a'
print(List[int], Optional[Dict[str, int]], Box[int](3).v, P(1), P._fields)
print(typing.get_type_hints(Box.__init__), Callable[[int], str], Union[int, str])
def f(a: List[int], b: 'Optional[str]' = None) -> Dict[str, int]:
    return {}
print(typing.get_type_hints(f))
