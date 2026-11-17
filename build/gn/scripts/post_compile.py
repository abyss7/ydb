#!/usr/bin/env python3
"""Launch the compiler, normalize its depfile and compress its time trace.

Depfile normalization uses the existing relative_deps.sh helper.

Usage: post_compile.py <source root> [--time-trace=FORMAT] [wrapper args...] -- <compiler args...>
"""

import os
import subprocess
import sys

from compile_time_traces import compress_trace, discard_trace


def absolute(path, directory):
    if path == "-" or os.path.isabs(path):
        return path
    return os.path.normpath(os.path.join(directory, path))


def trace_path(argv, cwd):
    enabled, value, output, source, dumpdir = False, None, None, None, None
    for i, arg in enumerate(argv):
        if arg == "-ftime-trace":
            enabled, value = True, None
        elif arg.startswith("-ftime-trace="):
            enabled, value = True, arg.split("=", 1)[1]
        elif arg == "-fno-time-trace":
            enabled = False
        elif arg in ("-o", "-c", "-dumpdir") and i + 1 < len(argv):
            if arg == "-o":
                output = argv[i + 1]
            elif arg == "-c":
                source = argv[i + 1]
            else:
                dumpdir = argv[i + 1]
    if not enabled:
        return None
    if value is not None:
        path = absolute(value, cwd)
        if value.endswith("/") or os.path.isdir(path):
            if not output:
                return None
            path = os.path.join(path, os.path.splitext(os.path.basename(output))[0] + ".json")
        return path
    name = dumpdir + os.path.basename(source) if dumpdir and source else output
    return absolute(os.path.splitext(name)[0] + ".json", cwd) if name and name != "-" else None


def main():
    args = sys.argv[1:]
    if "--" not in args or args.index("--") < 1 or args[-1] == "--":
        sys.exit(__doc__.split("Usage: ", 1)[1].strip())
    separator = args.index("--")
    root, wrapper, command = args[0], args[1:separator], args[separator + 1:]
    trace_format = "none"
    if wrapper and wrapper[0].startswith("--time-trace="):
        trace_format = wrapper.pop(0).split("=", 1)[1]
        if trace_format not in ("none", "json", "lz4", "zstd"):
            sys.exit("post_compile: invalid time trace format: " + trace_format)
    directory = os.getcwd()
    root = os.path.abspath(root)
    trace = trace_path(command, directory) if trace_format != "none" else None
    if trace:
        discard_trace(trace)
    depfile_helper = os.path.join(os.path.dirname(os.path.abspath(__file__)), "relative_deps.sh")
    result = subprocess.call(["sh", depfile_helper, directory + "/", root + "/",
                              os.path.relpath(root, directory) + "/"] + wrapper + command)
    if trace:
        if result != 0:
            discard_trace(trace)
        elif not os.path.isfile(trace):
            print("post_compile: missing time trace (compiler cache hit?): " + trace, file=sys.stderr)
        elif trace_format in ("lz4", "zstd"):
            try:
                compress_trace(trace, trace_format)
            except (OSError, subprocess.SubprocessError) as error:
                print("post_compile: keeping JSON trace %s: %s" % (trace, error), file=sys.stderr)
    return result if result >= 0 else 128 - result


if __name__ == "__main__":
    sys.exit(main())
