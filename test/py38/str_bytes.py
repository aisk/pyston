s = "héllo wörld"
print(s, len(s), s.upper(), s.title(), s[1], s[-1], s[::-1], s[2:5])
print(s.encode(), s.encode("latin-1"), b"abc".decode(), "€".encode("utf-8"), len("𝄞"), ord("𝄞"), chr(0x1F600))
print("a,b,,c".split(","), " x y ".split(), "abc".partition("b"), "-".join(["a", "b"]), "aXbXc".replace("X", "--"))
print("abc".startswith(("x", "a")), "Hello".swapcase(), "  pad ".strip(), "x".center(5, "*"), "42".zfill(5))
print("{} {:>5} {name!r}".format(1, "r", name="n"), f"{1 + 1} {'q'!r} {3.14159:.2f} {{}}", f"{s=}" if False else "")
x = 10
print(f"x={x:>4}|{x!s}|{[x, x]}|{x * 2:{x}}|")
print(repr("a'b\"c\n"), ascii("é"), str(b"x"), repr(b"\x00\xff"), "%s|%-4s|%04d" % ("a", "b", 7))
b = b"hello"
print(b[0], b[1:3], b + b"!", b * 2, b.upper(), b.split(b"l"), bytes([65, 66]), bytes(3), b.hex(), bytes.fromhex("4142"))
ba = bytearray(b"abc")
ba.append(100)
ba[0] = 65
print(ba, bytes(ba), len(ba), ba.decode())
print(sorted("hello"), list(map(str.upper, "ab")), "".join(reversed("abc")), "abc" < "abd", "b" in "abc")
print("x".isidentifier(), "1".isdigit(), "ß".upper(), "ǅ".lower(), "Σ".casefold(), "tab\there".expandtabs(4))
print(str.maketrans("ab", "xy"), "abc".translate(str.maketrans("ab", "xy")), "a%sb" % "-", "%(k)s" % {"k": "v"})
print(hash("abc") == hash("abc"), "abc".__hash__() == hash("abc"), str(123), str([1, "a"]), int("٣"))
try:
    b"x" + "y"
except TypeError as e:
    print("TypeError", e)
try:
    "é".encode("ascii")
except UnicodeEncodeError as e:
    print("UnicodeEncodeError", e)
