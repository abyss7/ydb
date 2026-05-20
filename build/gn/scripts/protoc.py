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
os.chdir(source_dir)
exit = subprocess.call(args)
if exit != 0: sys.exit(exit)

for output in outputs:
    with open(output, 'rt', encoding="utf-8") as f:
        patched_text, num_patches = patch_output(f.read())
    if num_patches:
        with open(output, 'wt', encoding="utf-8") as f:
            f.write(patched_text)
