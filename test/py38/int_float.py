print(1 + 2, 7 // 2, -7 // 2, 7 % 3, -7 % 3, 2 ** 10, 2 ** -1, 7 / 2)
print(2 ** 100, -(2 ** 70) // 3, (2 ** 70) % 7, hex(2 ** 65), int("123"), int("ff", 16), int(" 42 "))
print(1 << 70, (1 << 70) >> 68, 10 ** 20 // 10 ** 18, divmod(-7, 2), divmod(2 ** 80, 7))
print(abs(-5), abs(-2 ** 70), pow(3, 4, 5), pow(2, 100, 1000), round(2.5), round(3.5), round(1.23456, 2))
print(type(2 ** 100), type(1), isinstance(2 ** 100, int), (2 ** 100).bit_length(), (255).to_bytes(2, "big"))
print(int.from_bytes(b"\x01\x00", "little"), True + True, int(True), bool(0), bool(2 ** 70))
print(1.5 + 2, 0.1 + 0.2, 1e300 * 1e10, -1e300 * 1e10, float("inf"), float("-nan"), 3.0 // 2, 3.5 % 2)
print(float(2 ** 70), int(1e20), int(-3.9), (2.5).as_integer_ratio(), (0.1).hex(), float.fromhex("0x1.8p1"))
print(repr(1.0), repr(1e16), repr(1.1), str(1e-5), 1.0 == 1, hash(1.0) == hash(1), hash(2 ** 70) == hash(float(2 ** 70)))
print((-2) ** 0.5, 2 ** 0.5, 10 / 4, 1 / 3)
print(complex(1, 2) * 2, abs(3 + 4j), (1 + 2j).conjugate(), 1j ** 2, complex("1+2j"))
for x in [0, 1, -1, 2 ** 63, -(2 ** 63), 2 ** 64]:
    print(x, x - 1, x * 3, hash(x) == hash(float(x)))
print(sorted([3, 1.5, 2 ** 65, -1, True]), max(1, 2.5), min([5, 3, 9]), sum(range(10)), sum([0.1] * 10))
print(format(1234567, ","), format(3.14159, ".2f"), format(255, "#x"), format(10, "08b"), "%d %s %r %5.1f" % (1, "a", "b", 2.25))
print(int.__name__, int.__mro__, float.__mro__, bool.__mro__)
try:
    1 / 0
except ZeroDivisionError as e:
    print("ZeroDivisionError", e)
try:
    int("x")
except ValueError as e:
    print("ValueError", e)
