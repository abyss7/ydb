"""The plugins every executable links (//build/gn/link_slots.gni), gathered
from the BUILD.gn files of the tree.

A library names the targets built on top of it that it needs in the process
at run time -- implementations it finds in a registry (a static registration,
SRCS(GLOBAL ...) of ya.make), which reference the library, not the other way
round:

    library("engines") {
      plugins = [ "//ydb/core/tx/columnshard/engines/storage" ]
    }

A dependency on them would be a cycle. An executable declared with
linked_executable() or a template of //build/gn/testing.gni depends on the
plugins of every target it reaches instead: through deps and public_deps, the
providers of its link_select and the plugins themselves, until nothing new
comes up.

The files are parsed by gn itself, as in link_slot_index.py: a list must be
written literally in the target's block; a label with $-expansions, a target
created by a template with a computed name and the deps a template adds on
its own (the default ones) are not seen.

Usage: link_plugin_index.py <source root> <gn binary> [<build dir>]
(a build dir inside the source root is not scanned)
Prints a gn scope (for exec_script(..., "scope")):

    executables = [ { label = "//dir:name"  plugins = [ ... ] }, ... ]

The per-file parse is cached in <build dir>/link_plugin_index.cache.json and
reused until the file's stat or the gn binary changes: parsing the tree is
what takes the time, and a regen rewrites a few files only.
"""

import json
import os
import re
import shutil
import sys

import link_slot_index as lsi

KEY = "plugins"
EXECUTABLE = "linked_executable"
TESTING = "build/gn/testing.gni"   # the templates of tests, executables too
DEP_KEYS = ("deps", "public_deps")


def all_build_files(root, build_dir=None):
    out = []
    for d, dirs, files in lsi.walk(root, build_dir):
        if lsi.BUILD_FILE in files:
            out.append(os.path.relpath(os.path.join(d, lsi.BUILD_FILE), root))
    return out


def executables(root):
    with open(os.path.join(root, TESTING), encoding="utf-8") as f:
        return {EXECUTABLE} | set(re.findall(r'^template\("([^"]+)"\)', f.read(), re.M))


def mentions_plugins(root, files):
    key = re.compile(rb"^\s*" + KEY.encode() + rb"\s*\+?=", re.M)
    for f in files:
        with open(os.path.join(root, f), "rb") as fh:
            if key.search(fh.read()):
                return True
    return False


def _calls(node):
    """(function, name, block) of every call with a literal name and a block."""
    for st in node.get("child", []):
        t = st.get("type")
        if t == "FUNCTION":
            kids = st.get("child", [])
            if len(kids) == 2 and kids[0].get("type") == "LIST" and kids[1].get("type") == "BLOCK":
                args = kids[0].get("child", [])
                name = lsi._string(args[0]) if len(args) == 1 else None
                if name is not None:
                    yield st.get("value"), name, kids[1]
        elif t == "CONDITION":
            for sub in st.get("child", [])[1:]:
                if sub.get("type") in ("BLOCK", "CONDITION"):
                    yield from _calls(sub if sub["type"] == "BLOCK" else {"child": [sub]})


def _strings(block, key):
    """Plain strings of `key`, the rest (computed values) skipped."""
    out = []
    for st in block.get("child", []):
        t = st.get("type")
        if t == "BINARY" and st.get("value") in ("=", "+="):
            lhs, rhs = st["child"]
            if lhs.get("type") == "IDENTIFIER" and lhs.get("value") == key:
                items = rhs.get("child", []) if rhs.get("type") == "LIST" else [rhs]
                out += [s for s in map(lsi._string, items) if s is not None]
        elif t == "CONDITION":
            for sub in st.get("child", [])[1:]:
                if sub.get("type") == "BLOCK":
                    out += _strings(sub, key)
                elif sub.get("type") == "CONDITION":
                    out += _strings({"child": [sub]}, key)
    return out


def resolve(file, label):
    """A label as written in `file` -> //dir:name, or None (toolchain, $)."""
    if "(" in label:
        label = label[:label.index("(")]
    here = os.path.dirname(file).replace(os.sep, "/")
    if label.startswith(":"):
        d, name = here, label[1:]
    else:
        d, _, name = label.partition(":")
        d = d[2:] if d.startswith("//") else os.path.normpath(os.path.join(here, d)).replace(os.sep, "/")
        name = name or os.path.basename(d)
    return "//%s:%s" % (d, name)


def full(label):
    """//dir -> //dir:dir, the form resolve() gives."""
    d, _, name = label[2:].partition(":")
    return "//%s:%s" % (d, name or os.path.basename(d))


def short(label):
    d, _, name = label[2:].partition(":")
    return "//%s" % d if name == os.path.basename(d) else label


CACHE = "link_plugin_index.cache.json"
CACHE_VERSION = 1


def gn_stamp(gn):
    """[real path, mtime, size] of the gn binary, or None: another gn may
    parse the same file differently."""
    path = gn if os.path.isabs(gn) else shutil.which(gn)
    if not path:
        return None
    try:
        st = os.stat(path)
    except OSError:
        return None
    return [os.path.realpath(path), st.st_mtime_ns, st.st_size]


def file_stats(root, files):
    """{file: [mtime, size]} of the files that exist."""
    out = {}
    for f in files:
        try:
            st = os.stat(os.path.join(root, f))
        except OSError:
            continue
        out[f] = [st.st_mtime_ns, st.st_size]
    return out


def parse_files(root, gn, files):
    """{file: [[func, label, deps, plugins, link_select], ...]} as gn sees it."""
    out = {}
    for file, tree in lsi.dump_trees(root, gn, files):
        rows = []
        for func, name, block in _calls(tree):
            rows.append([func, full(lsi.label(file, name)),
                         [resolve(file, l) for k in DEP_KEYS for l in _strings(block, k)],
                         [resolve(file, l) for l in _strings(block, KEY)],
                         _strings(block, "link_select")])
        out[file] = rows
    return out


def load_cache(path, stamp, stats):
    """{file: targets} of the cache entries that still match stats."""
    if not path:
        return {}
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        if data.get("version") != CACHE_VERSION or data.get("gn") != stamp:
            return {}
        return {f: e[2] for f, e in data["files"].items()
                if f in stats and isinstance(e, list) and len(e) == 3 and e[:2] == stats[f]}
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        return {}


def save_cache(path, stamp, stats, targets):
    """The parsed files into the cache, atomically: two toolchains run this."""
    if not path:
        return
    data = {"version": CACHE_VERSION, "gn": stamp,
            "files": {f: stats[f] + [t] for f, t in targets.items() if f in stats}}
    tmp = "%s.%d.tmp" % (path, os.getpid())
    try:
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(data, f, separators=(",", ":"))
        os.replace(tmp, path)
    except OSError:
        try:
            os.remove(tmp)
        except OSError:
            pass


def index(root, gn, build_dir=None):
    # Parsing the tree (`gn format --dump-tree`) is the slow part, ~10 s of a
    # `gn gen`. The facts of one BUILD.gn do not depend on the others, so they
    # are kept in the build dir, keyed by their stat: a regen that rewrites a
    # few files re-parses those few, the closure is then computed from the
    # cache. Nothing points gn at the file: no build input, no build output,
    # it just goes away with the dir (a broken or other-version cache is
    # re-parsed, never trusted).
    files = all_build_files(root, build_dir)
    cache = os.path.join(build_dir, CACHE) if build_dir else None
    stamp = gn_stamp(gn) if cache else None
    stats = file_stats(root, files) if stamp else {}
    targets = load_cache(cache, stamp, stats) if stamp else {}
    if not targets and not mentions_plugins(root, files):
        return {}
    todo = [f for f in files if f not in targets]
    if todo:
        targets.update(parse_files(root, gn, todo))
        save_cache(cache, stamp, stats, targets)
    exes = executables(root)
    deps, plugins, selects = {}, {}, {}
    for rows in targets.values():
        for func, me, d, p, s in rows:
            deps[me] = d
            if p:
                plugins[me] = p
            if func in exes:
                selects[me] = s
    providers = lsi.index(root, gn, build_dir)[0]

    out = {}
    for exe, select in selects.items():
        queue = list(deps.get(exe, []))
        for entry in select:
            slot, _, value = entry.partition("=")
            queue += [full(l) for l in providers.get((slot, value), [])]
        seen, found = set(), []
        while queue:
            t = queue.pop()
            if t in seen:
                continue
            seen.add(t)
            queue += deps.get(t, [])
            for p in plugins.get(t, []):
                if p not in found:
                    found.append(p)
                queue.append(p)
        if found:
            out[exe] = sorted(short(p) for p in found)
    return out


def main():
    root, gn = sys.argv[1], sys.argv[2]
    build_dir = sys.argv[3] if len(sys.argv) > 3 else None
    print("executables = [")
    for exe, found in sorted(index(root, gn, build_dir).items()):
        print("  {\n    label = %s\n    plugins = [ %s ]\n  }," % (
            lsi._gn_string(exe), ", ".join(map(lsi._gn_string, found))))
    print("]")


if __name__ == "__main__":
    main()
