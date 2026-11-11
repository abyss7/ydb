"""Prints the number of CPUs a build may use, divided by the optional argument:
what ninja -j defaults to (respecting the affinity: cgroups, taskset), halved
here because //build/gn/config/CONFIG.gn cannot divide."""

import os
import sys

try:
    count = len(os.sched_getaffinity(0))
except AttributeError:  # no affinity on this platform
    count = os.cpu_count() or 1

divisor = int(sys.argv[1]) if len(sys.argv) > 1 else 1
print(max(1, count // divisor))
