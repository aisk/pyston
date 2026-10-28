# Times one benchmark in-process: the first iteration includes JIT compilation
# (and CPython's specialization warmup), the steady time is the best iteration
# after the warmup ones.
# usage: harness.py <bench> [warmup] [iters]
import os
import sys
import time

name = sys.argv[1]
if name.endswith(".py"):
    name = name[:-3]
warmup = int(sys.argv[2]) if len(sys.argv) > 2 else 3
iters = int(sys.argv[3]) if len(sys.argv) > 3 else 5

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
mod = __import__(name)

times = []
for i in range(warmup + iters):
    t0 = time.perf_counter()
    mod.run()
    times.append(time.perf_counter() - t0)

print("%s first=%.4f steady=%.4f" % (name, times[0], min(times[warmup:])))
