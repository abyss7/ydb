import os
import re
import subprocess
import sys


OUTPUTS_DELIMITER = "--"
source_dir = os.path.abspath(sys.argv[1])


# modifies in-place all arguments that look like path
def make_absolute_path(args):
    args[:] = [os.path.relpath(os.path.abspath(arg), source_dir) if not arg.startswith('-') else arg for arg in args]


# returns outputs and splits args
def parse_outputs(args):
    del_index = args.index(OUTPUTS_DELIMITER) if OUTPUTS_DELIMITER in args else len(args)
    outputs = args[:del_index]
    make_absolute_path(outputs)
    del args[:del_index + 1]
    return outputs


# replaces `--proto-include-paths-file FILE` with `-I PATH` pairs read from FILE.
def expand_include_paths_files(args):
    result = []
    i = 0
    while i < len(args):
        if args[i] == "--proto-include-paths-file":
            with open(args[i + 1], "rt", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if line:
                        result.extend(["-I", line])
            i += 2
        else:
            result.append(args[i])
            i += 1
    return result


# protoc derives a file's canonical name (used to mangle descriptor table
# symbols) from the first `-I` whose directory is a prefix of the cmd-line
# path. The default `-I .` is a prefix of every source, so without any help
# protoc picks the long form (e.g. `contrib/libs/foo/google/api/x.proto`).
# But `import "google/api/x.proto"` from a sibling resolves through a more
# specific `-I contrib/libs/foo` and gets the short canonical name. The two
# don't match → the importer references `descriptor_table_<short>` while the
# importee defines `descriptor_table_<long>`, and link/compile fails.
#
# Fix: pass the source file in the form that matches the most specific `-I`
# it lives under. Then both direct compilation and `import` resolution agree
# on the same canonical name. Sources not inside any non-root `-I` are left
# alone (their long form via `-I .` stays consistent with itself).
def relativize_proto_sources(args, source_dir):
    includes = []
    i = 0
    while i < len(args):
        if args[i] == "-I" and i + 1 < len(args):
            includes.append(args[i + 1])
            i += 2
        else:
            i += 1
    source_dir_abs = os.path.abspath(source_dir)
    abs_includes = []
    for inc in includes:
        abs_inc = os.path.normpath(os.path.join(source_dir_abs, inc))
        # Skip the source root: it matches every file and would defeat the
        # purpose of picking the most specific `-I`.
        if abs_inc == source_dir_abs:
            continue
        abs_includes.append(abs_inc)
    abs_includes.sort(key=len, reverse=True)
    for idx, arg in enumerate(args):
        if arg.startswith("-") or not arg.endswith(".proto"):
            continue
        abs_arg = os.path.normpath(os.path.join(source_dir_abs, arg))
        for abs_inc in abs_includes:
            inc_norm = abs_inc.rstrip("/") + "/"
            if abs_arg.startswith(inc_norm):
                args[idx] = abs_arg[len(inc_norm):]
                break


# returns patched content and number of changes.
def patch_output(content):
    num_patches = 0
    patches = [
        (re.compile(r"((?:struct|class)\s+\S+\s+)final\s*:"), r"\1:"),
        # (re.compile(r'(#include.*?)(\.proto\.h)"'), r'\1.pb.h"')
    ]
    for from_re, to_re in patches:
        content, n = re.subn(from_re, to_re, content)
        num_patches += n
    return content, num_patches


# Main script
args = sys.argv[2:]
outputs = parse_outputs(args)
args = expand_include_paths_files(args)
make_absolute_path(args)
relativize_proto_sources(args, source_dir)
os.chdir(source_dir)
exit = subprocess.call(args)
if exit != 0: sys.exit(exit)

for output in outputs:
    with open(output, 'rt', encoding="utf-8") as f:
        patched_text, num_patches = patch_output(f.read())
    if num_patches:
        with open(output, 'wt', encoding="utf-8") as f:
            f.write(patched_text)
