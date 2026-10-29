# Speculation on iterator and subscript types, including the cases where it turns out wrong.

def nested(n):
    s = 0
    for j in range(n):
        for i in range(100):
            s += i
    return s

def index_sum(a, n):
    s = 0
    for k in range(n):
        for i in range(-3, 3):
            s += a[i]
    return s

def iters(seqs):
    # The inner loop sees range iterators for a while, then other iterators.
    out = []
    for seq in seqs:
        t = 0
        for x in seq:
            t += x
        out.append(t)
    return out

def items(a):
    # a[i] is an int for a while, then a float and a str.
    out = []
    for k in range(len(a)):
        for i in range(200):
            v = a[k][i % len(a[k])]
            if i == 199:
                out.append(v)
    return out

def gen(n):
    for i in range(n):
        yield i * 2

class L(list):
    def __getitem__(self, i):
        return 7

def out_of_range(a, n):
    for k in range(n):
        for i in range(10):
            try:
                a[i]
            except IndexError as e:
                if k == n - 1:
                    print(i, e)
                break

print(nested(3000))
a = [1, 2, 3, 4, 5, 6]
print(index_sum(a, 5000), index_sum(L(a), 3000))
print(iters([range(500)] * 50 + [[1, 2, 3], (4, 5), gen(100), iter(range(10 ** 20, 10 ** 20 + 3)), range(50)]))
print(items([[1, 2, 3]] * 30 + [[1.5, 2.5], ["x", "y"], [1, 2]]))
out_of_range(a, 3000)
