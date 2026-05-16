#!/usr/bin/env python3
"""Thin wrapper around contrib/tools/python3/Programs/_freeze_module.

That tool produces a C header containing the marshaled bytecode of a single
Python source file. It takes positional arguments (name, input, output) and
does not derive the module's dotted name from the input path itself — the
GN action passes it explicitly via --name.

This wrapper exists so the GN action signature is uniform with the rest of
our build scripts (flagged args + working directory handling) and so we can
add cleanup/diagnostics later without touching every BUILD.gn.
"""

import argparse
import os
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--freeze", required=True,
                        help="Path to the _freeze_module executable")
    parser.add_argument("--name", required=True,
                        help="Dotted module name, e.g. 'json.encoder'")
    parser.add_argument("--input", required=True,
                        help="Source .py file")
    parser.add_argument("--output", required=True,
                        help="Destination .h file")
    args = parser.parse_args()

    os.makedirs(os.path.dirname(args.output), exist_ok=True)

    result = subprocess.run(
        [args.freeze, args.name, args.input, args.output],
        check=False,
    )
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
