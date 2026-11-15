#!/usr/bin/env python3
"""A digest of the compile commands the link-slot weak references borrow
their flags from (build/gn/scripts/link_slot_weak.py), for
//build/gn/config:cc_digest.

Each command is reduced to what link_slot_weak.py borrows of it: its input,
output, dependency file and the precompiled/force-included headers drop out
(the _DROP* of link_slot_weak.compile_argv), the rest is the flags. The
digest is over the sorted set of the commands' reductions: a new source
built the same way leaves it alone, a new flag (an include path, a define:
new mangled names in the slot headers) changes it.

The link_slot_weak actions (link_slot_weak_refs in //build/gn/link_slots.gni)
take the digest as an input instead of compile_commands.json itself: gn
rejects a file of the build dir in `inputs` unless a target generates it,
and that file a gen rewrites every time, which would rerun every slot's
AST dumps on every gen. The digest action reads the commands undeclared the
same way and names them in its depfile: a gen reruns the digest alone (a
hash, not the dumps), and unless the flags really changed, the digest comes
out the same and keeps its mtime -- the weak actions stay clean.
"""

import argparse
import hashlib
import json
import re


def signature(command):
    """The flags of one compile command: what link_slot_weak.py borrows of
    it, spelled the same way (the order inside the command kept, only the
    per-file tokens gone)."""
    s = re.sub(r"-Xclang -include -Xclang \S+", " ", command)   # a slot's weak refs
    for flag in ("-MF", "-MQ", "-MT", "-o", "-include-pch", "-include", "-c"):
        s = re.sub(r"(?<!\S)" + re.escape(flag) + r" \S+", " ", s)
    return " ".join(s.split())


def digest(entries):
    """Over the flag signatures of all the commands, duplicates merged."""
    h = hashlib.sha256()
    for s in sorted({signature(e.get("command") or " ".join(e["arguments"]))
                     for e in entries}):
        h.update(s.encode())
        h.update(b"\0")
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compile-commands", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("--depfile", required=True)
    args = ap.parse_args()

    with open(args.compile_commands, encoding="utf-8") as f:
        text = digest(json.load(f))

    clean = False
    try:
        with open(args.output, encoding="utf-8") as f:
            clean = f.read() == text   # an unchanged digest keeps its mtime:
    except OSError:                     # the weak actions stay clean
        pass
    if not clean:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(text)
    with open(args.depfile, "w", encoding="utf-8") as f:
        f.write("%s: %s\n" % (args.output, args.compile_commands))


if __name__ == "__main__":
    main()
