# Porting Pyston to Python 3.8

The target is CPython 3.8.20 (the final 3.8 release, `v3.8.20` in the CPython repo).
A reference interpreter for the tests can be built from that tag into
`~/pyston_deps/python3.8`.

The goal of the first round is the core language and builtin types. The stdlib and
the test suite are cut down to what is needed to validate the core, and get filled
back in later. Changes that only buy a small amount of compatibility at a large cost
are deferred and listed at the end of this file.

## Design decisions

- **Front end.** The 3.8 pgen parser, tokenizer and `Python/ast.c` are used as is.
  `codegen/cpython_ast.cpp` converts the 3.8 AST into Pyston's AST, and `core/cfg.cpp`
  lowers the new constructs into BST. Before converting, CPython's `Python/symtable.c`
  is run on the AST only to raise the scoping SyntaxErrors CPython's compiler would
  (global/nonlocal misuse, assignment expressions in comprehensions). Pyston's own
  scoping analysis still decides how names are bound.
- **`str`** is the 3.8 PEP 393 unicode object from `Objects/unicodeobject.c`, so C
  modules that use the `PyUnicode_*` macros work unchanged. `BoxedString` is a C++
  view over that layout. `s()` returns a UTF-8 `StringRef`; for compact ASCII strings
  (nearly all identifiers) it points straight at the character data. Pyston's intern
  table backs `PyUnicode_InternInPlace`, so interned names still compare by pointer.
- **`bytes`** is `Objects/bytesobject.c` from 3.8. Pyston 2.7's C++ `str` (a byte string, so
  the natural match for `bytes`) is gone. Bringing it back as `bytes` was considered and left
  for later: the hot strings in Python 3 (identifiers, attribute names, dict keys) are `str`,
  so a C++ `bytes` buys little, while a rewrite would drift from CPython's `bytesobject.c` and
  make moving to a newer CPython harder. String performance work should target `str`.
- **`int`** keeps Pyston's C++ small int (`BoxedInt`) so the JIT fast paths survive.
  Values that do not fit in 64 bits are `BoxedLong` (GMP) instances of a hidden
  subclass `long_cls`. Arithmetic results that fit in 64 bits are turned back into
  `BoxedInt`, and `PyLong_CheckExact` accepts both. `type()` and `__class__` report `int`
  for the hidden subclass (`visibleClass()`), so code that dispatches on the exact type
  (`copy`, `pickle`) works. int's methods forward to the GMP versions when `self` or the
  other operand is big, and their specialized versions only accept exact small ints.
  `Py_TYPE()` in C code still sees the hidden class. Merging the two into one layout is
  still an open decision.
- **`range`** is `Objects/rangeobject.c`, since Python 3's range (big ints, slicing,
  `count`/`index`, comparisons) is much more than Pyston's old `xrange`. Its iterator for
  ranges that fit in a C long gets Pyston's iteration protocol in
  `runtime/inline/range.cpp` (`tpp_hasnext`, `__hasnext__`, an unboxed `__next__`), which
  is part of the inlined runtime bitcode. `range` and `range_iterator` are marked
  `is_constant` so the JIT can speculate on them.
- Old-style classes, `print`/`exec` statements, backticks, `long`, `basestring`,
  `unicode`, `xrange`, `file` and `__cmp__` are removed.
- C extension modules from 3.8 are compiled in, and are initialized through
  `PyModuleDef` (`PyInit_*`).

- **Positional-only arguments** (`/`) are the first `ParamNames::num_posonly` arguments.
  Keyword matching skips them, so a keyword with the same name goes to `**kwargs`.
- **Keyword-only arguments.** `BoxedCode::num_args` counts them, and they are the last
  `num_kwonly` of the arguments (`ParamNames` uses CPython's order: positional, keyword-only,
  `*args`, `**kwargs`). Their defaults live in the function's `kwdefaults` dict
  (`__kwdefaults__`). The rewriting fast paths of `rearrangeArguments` are skipped for
  functions that have them.
- **New syntax is lowered in the CFG** rather than getting new BST nodes, so the three
  execution tiers need few changes:
  - `yield from` is a loop around the `yield_from_iter` and `yield_from_step` runtime
    helpers. Throwing into and closing the delegated generator is not forwarded yet.
  - Starred displays (`[*a, b]`, `{*a}`, `(*a,)`) build a list or set with
    append/extend/add/update. Starred assignment calls `unpack_starred`.
  - `{**a}` and calls with several or non-final `*args`/`**kwargs` are rewritten into
    the Python 2 call form with a starred tuple display and a dict display.
  - `nonlocal` is a `Global` node with a flag. Stores to such names are DEREF stores, and
    the owning function reads them back from the closure it created.
  - `raise X from Y` calls the `raise_from` helper, which builds the exception and sets
    its cause, and then raises the result.
  - `@` is a new pseudo binop type (`AST_TYPE::MatMult`) next to `TrueDiv` and `DivMod`.
  - List comprehensions get their own function scope, like set and dict comprehensions
    (they were inlined with Python 2 semantics before).
  - `x := value` is an `AST_NamedExpr` that the CFG lowers to an assignment. Inside a
    comprehension the scoping analysis marks the target nonlocal (or global at module level)
    in the comprehension scopes and written in the enclosing function, as PEP 572 says.
  - `continue` is allowed inside `finally`.
- **`__qualname__`** is computed in `ModuleCFGProcessor` and stored on `BoxedCode`.
  Class bodies get a `__qualname__` assignment like in CPython.
- **Closures still don't use cells.** `__closure__` returns a snapshot tuple of `cell`
  objects (from `cellobject.c`) in `co_freevars` order. Writes to those cells are not
  seen by the function.
- **Zero-argument `super()`** finds the class at runtime from the frame's first argument,
  by walking its MRO for the class whose dict holds the running function. There is no
  `__class__` cell.
- **`async def`** compiles to a function that raises `SystemError`, so modules that only
  define coroutines still import. `types.py` and `_collections_abc.py` have Pyston
  changes for this.
- **Exception state follows Python 3 without changing how exceptions are thrown** (still a
  C++ throw of a type/value/traceback triple). Each frame keeps its handled exception in
  `FrameInfo::exc`; NULL there means "inherited from the caller". The CFG saves it with
  `get_exc_value` before every `try` with handlers and every `with`, and handlers restore it
  with `restore_exc_info` on fall-through, `break` and `continue`. Saving at the `try` rather
  than in the handler keeps it right when an inner handler raised. Finally blocks and
  `__exit__` see the propagating exception. A `yield` inside a handler keeps the frame's
  state; other yields drop it so a resumed generator sees its new caller's.
  `__context__` is set when an exception without a traceback (that is, newly raised) reaches
  its first Python frame (`chainExceptionContext`, called from `raise3`, `raise3_capi`, the
  unwinder and `caughtCapiException`). `__traceback__` is set by `setFrameExcInfo`.
- **Compile-time checks from CPython's compiler** (SyntaxWarnings for `is` with a literal,
  `assert (x, msg)`, missing commas) are emitted while converting the AST.
- **`slot_tp_richcompare`** only calls the type's own method, as in Python 3; the reflected
  method is tried by `do_richcompare`. `object.__reduce_ex__` uses CPython 3.8's
  `reduce_newobj` (`__getnewargs_ex__`, `copyreg.__newobj_ex__`).
- **`object` has CPython's `tp_richcompare`**, so it has `__eq__`/`__ne__` and the
  default `__ne__` inverts `__eq__`.
- **`dict` is ordered.** `BoxedDict::DictMap` is an `OrderedDenseMap` (core/ordered_dense_map.h):
  an entry array in insertion order plus a DenseMap from key to position, like CPython's
  compact dict. Hidden classes use the same map for their attribute offsets, so instance and
  class `__dict__` also iterate in insertion order. `PyDict_Next` stores the entry position in
  its cursor, and `popitem()` removes the last item.
- **Annotations** are handled in `codegen/cpython_ast.cpp`. Module and class bodies with
  annotated assignments start with a `SETUP_ANNOTATIONS` primitive (the `setup_annotations`
  helper, which keeps an existing `__annotations__`) and store each annotation there, with
  private names mangled. In a function, `x: int` becomes `if 0: x = None` so x is local.
  A def's annotations are set with `f.__annotations__ = {...}` right after it; when it has
  decorators, their expressions are evaluated into temporaries before the def and applied
  after the annotations are set.
- **`del` of a closure variable** clears the closure slot through `delClosureVar`. The
  baseline JIT doesn't handle it and leaves those functions to the interpreter.
- **The stdlib is kept minimal**: the modules needed at startup and the core ones the tests
  use (`collections`, `functools`, `re`, `enum`, `typing`, `threading`, ...). Only
  `types.py` and `_collections_abc.py` carry Pyston changes. Modules are added when a core
  feature needs them, not to run CPython's test suite, since the stdlib will be replaced
  again when moving past 3.8.
- **PEP 560**: subscripting a class falls back to its `__class_getitem__`, and
  `createUserClass` resolves `__mro_entries__` like CPython's `update_bases`.
- **Numeric hashes** follow Python 3 (reduction modulo `2**61 - 1`), so equal ints and
  floats hash the same.

## Testing

`test/py38/run.sh` runs each test in `test/py38` with CPython 3.8 and with
`build/P38Debug/pyston` and diffs the output. Pass `-n` to force the LLVM tier. The
reference interpreter path can be overridden with `CPYTHON=...`.

CPython's own test suite is only a reference. It needs `unittest`, `test.support` and a
large part of the stdlib, so it is not run. When working on a feature, read the matching
tests in `~/cpython38/Lib/test` and copy the relevant cases into a small `test/py38` test.

The command line takes CPython's `-I`, `-s`, `-W` and `-X` (the last two only fill
`sys.warnoptions` and `sys._xoptions`). Pyston's old `-I` (interpreter only) is now `-Y`.

Things that are easy to get wrong while working on the port:

- The stdlib copy and the C source lists are globbed when CMake configures, so run
  `cmake .` in `build/P38Debug` after adding files under `from_cpython`.
- Don't rebuild while tests run; the tests use the binary and `lib/` in the build dir.
- Check a semantic change in all three tiers: `-Y`, the default, and `-n`. The tiers use
  different entry points for the same operation, for example the LLVM tier raises through
  `raise3_capi` while the interpreter uses `raise3`, and C++ exceptions get their traceback
  in the unwinder (`codegen/unwinding.cpp`) rather than in `caughtCxxException`.
- To call runtime code from the CFG, make a builtin function object once at startup and
  call it with `makeCall(TmpValue(addConst(incref(func)), lineno), args)` (see
  `raise_from_func`, `get_exc_value_func`). This needs no changes in the three tiers.
- In `core/cfg.cpp`, `_dup()` emits an assignment, so compute every `_dup()` before the
  `allocAndPush` of the node that uses them, or the copies land after it.
- CPython checks that live in its compiler or symbol table belong in
  `codegen/cpython_ast.cpp` (warnings, annotation handling) or come from `symtable.c`.

## Performance to verify

The port has not been benchmarked yet. These are the changes most likely to have moved
performance compared to the 2.7 build, and should be measured once benchmarks run on 3.8
(the scripts in `microbenchmarks/` and `minibenchmarks/` are still Python 2 and need
porting first).

- **`str` is CPython's unicode object.** Pyston's own C++ `str` (about 2900 lines in
  `runtime/str.cpp`) and the JIT's fast paths for it are gone. Non-ASCII strings pay for a
  UTF-8 conversion the first time Pyston's C++ code asks for `s()`. Worth measuring
  string building, slicing, formatting, `str.join`, and attribute lookups with non-ASCII
  names.
- **`bytes` is CPython's `bytesobject.c`** instead of Pyston's `str`.
- **Two int layouts behind one `int` type.** Results of big-int operations are checked and
  turned back into small ints (`normalizeLong`), and `int` methods forward big values to
  the GMP subclass. Measure mixed small and big int arithmetic and `int` dict keys.
- **Python 3 numeric hashing** reduces ints modulo `2**61 - 1`, which adds work to every
  hash of an int key.
- **Exception state is saved at every `try` with handlers and every `with`** through a
  call to a runtime helper (`get_exc_value`), and handlers restore it on exit. `except ...
  as e` adds a store and a delete, and finally blocks set the exception info on their
  exception path. The helper is cheap when the frame has no exception state of its own,
  but it is still a call per `try` entry where CPython has none. A dedicated BST opcode
  would remove it. Measure tight loops with `try` and `with`.
- **Class bodies run in the namespace from `__prepare__`**, handed to the body's frame
  through a thread-local (`createClassNamespace`), so class creation costs an extra call.
  Name stores in a class body go through `setitem` on that namespace.
- **Every parse runs CPython's symbol table pass** in addition to Pyston's scoping
  analysis, which costs some time on module import and `exec`/`eval`.
- **Exception chaining** looks up the exception being handled (`getFrameExcInfo`) for every
  newly raised exception that reaches a Python frame, and sets `__context__`. Measure
  `exceptions_ubench.py` style loops.
- **`range` loops.** `__iter__` can return either `range_iterator` or
  `longrange_iterator`, so the JIT only learns the iterator type at run time, and creating a
  range goes through CPython's argument parsing. On a 1M iteration `for i in range(n): pass`
  loop the 3.8 Release build takes about 1.2x the time of the 2.7 build with `xrange`, and
  short ranges created in a loop are slower still.
- **Types implemented in C aren't `is_constant`.** `freeze()` would replace their C slots, so
  `str`, `bytes` and the other CPython types are not marked constant and the JIT doesn't
  speculate on them (`range` is set by hand).
- **`dict` keeps a second array** for insertion order, so every dict costs more memory and
  inserts write two structures. Lookups go through the index DenseMap as before.
- **Calls with several `*args`/`**kwargs`** build an extra tuple and dict, and functions
  with keyword-only arguments skip the rewriting fast paths in `rearrangeArguments`.
- **`object.__eq__`/`__ne__` exist**, so comparisons of plain objects go through
  `object_richcompare` and the NotImplemented fallback instead of failing fast.
- **`__bool__`/`__len__` checks** are stricter in `nonzero`, adding a type check per call
  on user classes.
- **Zero-argument `super()`** walks the MRO at runtime on every call.
- **List comprehensions** are now a function call each time, like in CPython 3, instead
  of being inlined into the enclosing function.
- **The baseline JIT gives up** on functions that store to `nonlocal` names, and
  `yield from` is a loop around helper calls rather than a native primitive.
- **Mutation safety from CPython's tests.** `set.update`, `|=`, `-=` and friends iterate
  over a copied array of the other set's elements, `dict.__eq__` and `dict.update(dict)`
  walk by position holding references, dict iterators compare the size on every step, and
  calling a non-function object goes through a recursion depth check.

## Phases

1. Replace `from_cpython/Include` with the 3.8 headers and re-apply Pyston's changes
   (Box/BoxedClass layouts, `PYSTON_NOEXCEPT`, hcattrs, static types replaced with
   pointers to the C++ types).
2. Replace the C sources in `from_cpython` with their 3.8 versions, keeping only the
   modules needed at startup and by the core tests.
3. Port `src/` to the new headers and to Python 3 semantics, running the interpreter
   tier first, then the baseline JIT, then the LLVM tier.
4. Front end: new AST nodes (keyword-only args, `nonlocal`, starred assignment,
   `yield from`, f-strings, walrus, `@`, annotations, `raise from`, class keywords,
   zero-argument `super()`).
5. A minimal 3.8 `Lib` and a new core test directory, compared against CPython 3.8.

## Deferred

- async/await and coroutines (`async for`, `async with` are syntax errors).
- NFKC normalization of non-ASCII identifiers.
- Multi-phase module initialization beyond what the bundled modules need.
- Real cells for closures (`__closure__` is a read-only snapshot), and a `__class__` cell
  for zero-argument `super()`.
- `throw()`/`close()` delegation in `yield from`.
- Exceptions are still thrown as a (type, value, traceback) triple. `__context__`,
  `__cause__` and `__traceback__` work, but `except ... as e` is not unbound when the
  handler raises, and a `break` or `continue` out of a finally block that swallows an
  exception leaves that exception as the current one.
- Strings with lone surrogates get their surrogatepass UTF-8 cached when Pyston's
  internals need a `StringRef`, after which `str.encode()` no longer rejects them.
- Small gaps found in review: a lambda inside a comprehension gets qualname `<lambda>`.
- Duplicate keys across several `**kwargs` in one call are merged instead of raising.
- There is no hash randomization.
- `**kwargs` in class definitions and starred class bases.
- `__class__` is not a cell, so methods can't assign it with `nonlocal __class__`
  (CPython's test_super fails to compile).
- `int` subclasses can't hold values that don't fit in 64 bits.
- The baseline JIT gives up on functions that store to `nonlocal` names.
- `sys.settrace`/`setprofile` only store the function, nothing calls it.
- `PyType_FromSpecWithBases` supports a single base, and the type is immortal.
- `PyImport_Cleanup` is a stub, so the exit-time refcount leak check is off unless
  `PYSTON_ASSERT_REFS=1` is set.
- Some Python 2 attributes are still around on builtin types, e.g. `__hasnext__` (used
  by the JIT for loops) and `__base__` showing up in `dir()`. `reversed` is a function,
  not a type.
