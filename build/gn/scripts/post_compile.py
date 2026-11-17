#!/usr/bin/env python3
"""Launch the compiler, normalize its depfile and compress its time trace.

Clang writes chained PCH imports relative to -isysroot, but reads those
imports relative to cwd. Keep cwd equal to the source root and make the
paths in GN's build-directory-relative command absolute before changing it.
Parent PCH arguments stay relative to that root: Clang also records their
original spelling in its module-offset map. Ordinary compilations keep
their original argv and cwd. Rebase depfiles to the spellings Ninja uses.

Usage: post_compile.py <source root> [--global-pch] [--time-trace=FORMAT] [wrapper args...] -- <compiler args...>
"""

import os
import re
import subprocess
import sys

from compile_time_traces import compress_trace, discard_trace


# GN spells the source as `-c <source>`. Forwarded cc1 arguments may have an
# intervening -Xclang, e.g. -Xclang -include -Xclang gen/slot_weak.h.
PATH_OPTIONS = {
    "-c", "-o", "-MF", "-I", "-F", "-isystem", "-iquote", "-idirafter",
    "-iframework", "-iframeworkwithsysroot", "-isysroot", "--sysroot",
    "-include", "-include-pch", "-imacros", "-iprefix", "-iwithprefix",
    "-iwithprefixbefore", "-isystem-after", "-serialize-diagnostics", "-dumpdir",
}
JOINED_PATH_OPTIONS = (
    "--sysroot=", "-fmodule-map-file=", "-fmodules-cache-path=", "-ftime-trace=",
    "-include-pch=", "-isystem", "-iquote", "-idirafter", "-isysroot", "-I", "-F",
)


def absolute(path, directory):
    if path == "-" or os.path.isabs(path):
        return path
    return os.path.normpath(os.path.join(directory, path))


def executable(path, directory):
    return absolute(path, directory) if "/" in path else path


def uses_pch(argv):
    return any(a == "-include-pch" or a.startswith("-include-pch=")
               or a == "c++-header" for a in argv)


def absolute_command(argv, directory, root):
    out = [executable(argv[0], directory)]
    path_next = None
    for arg in argv[1:]:
        if path_next and arg != "-Xclang":
            path = absolute(arg, directory)
            if path_next == "-dumpdir" and arg.endswith("/"):
                path += "/"
            out.append(os.path.relpath(path, root) if path_next == "-include-pch" else path)
            path_next = None
        elif arg in PATH_OPTIONS:
            out.append(arg)
            path_next = arg
        else:
            for prefix in JOINED_PATH_OPTIONS:
                if arg.startswith(prefix) and arg != prefix:
                    path = absolute(arg[len(prefix):], directory)
                    if prefix == "-ftime-trace=" and arg.endswith("/"):
                        path += "/"
                    if prefix == "-include-pch=":
                        path = os.path.relpath(path, root)
                    out.append(prefix + path)
                    break
            else:
                out.append(arg)
    return out


def depfile_words(text):
    return [re.sub(r"\\(.)", r"\1", word).replace("$$", "$")
            for word in re.findall(r"(?:\\.|[^\s])+", text)]


def depfile_escape(path):
    return (path.replace("\\", "\\\\").replace("$", "$$").replace("#", "\\#")
            .replace(" ", "\\ ").replace("\t", "\\\t").replace(":", "\\:"))


def rebase_depfile(path, root, directory, cwd):
    def rebase(name):
        if cwd == directory and not os.path.isabs(name):
            return name
        full = absolute(name, cwd)
        if any(os.path.commonpath([full, base]) == base for base in (root, directory)):
            return os.path.relpath(full, directory)
        return full

    with open(path) as f:
        lines = f.read().replace("\\\n", "").splitlines()
    rules = []
    for line in lines:
        if not line.strip():
            continue
        targets, deps = re.split(r"(?<!\\):(?=\s|$)", line, maxsplit=1)
        rules.append(" ".join(depfile_escape(rebase(p)) for p in depfile_words(targets)) + ": " +
                     " ".join(depfile_escape(rebase(p)) for p in depfile_words(deps)) + "\n")
    with open(path, "w") as f:
        f.writelines(rules)


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
    global_pch, trace_format = False, "none"
    while wrapper:
        if wrapper[0] == "--global-pch":
            global_pch = True
        elif wrapper[0].startswith("--time-trace="):
            trace_format = wrapper[0].split("=", 1)[1]
            if trace_format not in ("none", "json", "lz4", "zstd"):
                sys.exit("post_compile: invalid time trace format: " + trace_format)
        else:
            break
        wrapper = wrapper[1:]
    directory = os.getcwd()
    root = os.path.abspath(root)
    cwd = directory
    if global_pch and uses_pch(command):
        command = absolute_command(command, directory, root)
        if wrapper:
            wrapper[0] = executable(wrapper[0], directory)
        cwd = root
    trace = trace_path(command, cwd) if trace_format != "none" else None
    if trace:
        discard_trace(trace)
    result = subprocess.call(wrapper + command, cwd=cwd)
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
    if result == 0 and "-MF" in command:
        depfile = absolute(command[command.index("-MF") + 1], cwd)
        if os.path.exists(depfile):
            rebase_depfile(depfile, root, directory, cwd)
    return result if result >= 0 else 128 - result


if __name__ == "__main__":
    sys.exit(main())
