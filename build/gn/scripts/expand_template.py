#!/usr/bin/env python3
"""Backs the expand_template() GN template.

Copies --input to --output, replacing every @KEY@ placeholder with the matching
value from the JSON map in --defines-file (CMake @ONLY semantics). Placeholders
without a matching key are left untouched.
"""

import argparse
import json
import os
import re
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--defines-file")
    args = parser.parse_args()

    defines = {}
    if args.defines_file:
        with open(args.defines_file, encoding="utf-8") as f:
            defines = json.load(f)

    with open(args.input, encoding="utf-8") as f:
        text = f.read()

    def replace(match):
        key = match.group(1)
        if key in defines:
            return str(defines[key])
        return match.group(0)

    text = re.sub(r"@([A-Za-z0-9_]+)@", replace, text)

    out_dir = os.path.dirname(args.output)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as f:
        f.write(text)


if __name__ == "__main__":
    sys.exit(main())
