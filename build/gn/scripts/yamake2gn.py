#!/usr/bin/env python3
"""Convert trivial ya.make modules to BUILD.gn (v1 + merge pass).

Scope:
  * Only modules OUTSIDE contrib/ are *generated* (contrib may still be a dep).
  * Only "trivial" modules are converted: ya.make using nothing beyond
    LIBRARY / PROGRAM / SRCS / PEERDIR / RECURSE / RECURSE_FOR_TESTS / END.
    Anything else (CFLAGS, YQL_LAST_ABI_VERSION, GENERATE_ENUM_SERIALIZATION,
    PROTO_LIBRARY, IF/ELSE, ADDINCL, tests, ...) -> SKIPPED and reported.
  * Headers dropped from `sources`.
  * deps derived from the real #include graph of the module's own files, NOT
    from PEERDIR (PEERDIR is only a cross-check diff in the report).
  * Classification: every include -> `deps`; if it appears in one of THIS
    module's *headers*, the module re-exports it -> promote to `public_deps`.
  * `util` (and util/**) is always-available and never emitted as a dependency.

Pass 2 (BUILD.gn merge), all-or-nothing + drop aggregator:
  A parent dir P absorbs its direct children into P/BUILD.gn iff EVERY direct
  real-module child of P is a leaf (no real modules nested below, tests ignored)
  AND was successfully generated. Child //P/C -> //P:C (label remap applied
  everywhere). A pure aggregator P (no own sources) loses its own target.
"""

import argparse
import os
import re
import subprocess
import sys
from collections import defaultdict

# --- ya.make macro vocabulary ---------------------------------------------

MODULE_MACROS = {"LIBRARY", "PROGRAM"}
KIND_MACROS = MODULE_MACROS | {"PROTO_LIBRARY"}

# SET(VAR ...) calls with these variable names are build-irrelevant metadata;
# silently ignored so they don't block conversion.
_IGNORED_SET_VARS = {
    "IDE_FOLDER",
    "PROTOC_TRANSITIVE_HEADERS",
}
TRIVIAL_MACROS = MODULE_MACROS | {
    "SRCS", "SRC", "PEERDIR", "RECURSE", "RECURSE_FOR_TESTS", "RECURSE_ROOT_RELATIVE", "END", "SUBSCRIBER",
    "GENERATE_ENUM_SERIALIZATION",   # -> serialize_enum_headers
    "SRCDIR", "ADDINCL", "YQL_LAST_ABI_VERSION", "NO_WSHADOW", "ENABLE", "NO_COMPILER_WARNINGS", "SUPPRESSIONS", "NEED_CHECK", "ENV",
    "GENERATE_ENUM_SERIALIZATION_WITH_HEADER", # TODO: temporary ignore
    "RESOURCE", "CFLAGS", "YQL_ABI_VERSION", "ALLOCATOR_IMPL", "GRPC", # TODO: temporary ignore
    "CHECK_DEPENDENT_DIRS", # TODO: temporary ignore
    "NO_UTIL",   # LIBRARY -> contrib_library (no default //util dep)
}
# PROTO_LIBRARY macros that are pure noise in GN (template always does grpc +
# --fatal_warnings, python/metadata/tags are irrelevant) -> drop, do not block.
PROTO_IGNORE_MACROS = {
    "PROTOC_FATAL_WARNINGS", "GRPC", "EXCLUDE_TAGS", "INCLUDE_TAGS", "ONLY_TAGS",
    "PY_NAMESPACE", "NO_OPTIMIZE_PY_PROTOS", "NO_MYPY", "LICENSE", "LICENSE_TEXTS",
    "WITHOUT_LICENSE_TEXTS", "VERSION", "SUBSCRIBER", "MAVEN_GROUP_ID",
    "ORIGINAL_SOURCE", "NO_COMPILER_WARNINGS", "ADDINCL", "YQL_LAST_ABI_VERSION",
}
PROTO_TRIVIAL_MACROS = {
    "PROTO_LIBRARY", "SRCS", "PEERDIR", "RECURSE", "RECURSE_FOR_TESTS", "END",
    "GENERATE_ENUM_SERIALIZATION",   # -> serialize_enum_headers (on generated .pb.h)
    "USE_COMMON_GOOGLE_APIS",   # -> public dep on googleapis-common-protos
} | PROTO_IGNORE_MACROS
# Proto-provided deps that the GN protobuf_library template already injects.
PROTO_PROVIDED_LABELS = {"//contrib/libs/protobuf", "//contrib/libs/grpc"}
# USE_COMMON_GOOGLE_APIS(...) pulls in google/api/*.proto etc., always imported
# (and thus re-exported) by the module's own .proto sources -> always public.
GOOGLEAPIS_COMMON_PROTOS_LABEL = "//contrib/libs/googleapis-common-protos"
# Macros that declare an artifact-producing (non-test) module.
ARTIFACT_MACROS = KIND_MACROS | {
    "PY3_LIBRARY", "PY23_LIBRARY", "PY3_PROGRAM",
    "GO_LIBRARY", "GO_PROGRAM", "RESOURCES_LIBRARY", "DLL",
}
TEST_MACROS = {
    "UNITTEST", "UNITTEST_FOR", "GTEST", "G_BENCHMARK", "Y_BENCHMARK",
    "PY2TEST", "PY3TEST", "PY23_TEST", "BOOSTTEST", "EXECTEST", "FUZZ",
}

HEADER_EXTS = (".h", ".hpp", ".hh", ".hxx", ".inc", ".cuh")
SOURCE_EXTS = (".cpp", ".cc", ".cxx", ".c")
PROTO_EXTS = (".proto",)
ARCH_SUFFIXES = ("_sse2", "_sse3", "_ssse3", "_sse41", "_sse42", "_avx", "_avx2",
                 "_avx512", "_pclmul")

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')
IMPORT_RE = re.compile(r'^\s*import\s+(?:public\s+|weak\s+)?"([^"]+)"')
PB_RE = re.compile(r'(\.grpc)?\.pb\.(h|cc)$')
MACRO_RE = re.compile(r'([A-Z][A-Z0-9_]*)\s*\(')

# A BUILD.gn carrying this directive in a comment is hand-maintained: the
# generator must never overwrite or delete it. Drop it anywhere in the file,
# e.g. on the first line:
#     # yamake2gn: keep  -- hand-written, do not regenerate
KEEP_DIRECTIVE_RE = re.compile(r'#[^\n]*\byamake2gn:\s*keep\b', re.IGNORECASE)


def buildgn_is_protected(path):
    """True if the BUILD.gn at `path` opts out of (re)generation via the
    `yamake2gn: keep` comment directive (missing file -> not protected)."""
    try:
        with open(path, encoding="utf-8", errors="ignore") as f:
            return bool(KEEP_DIRECTIVE_RE.search(f.read()))
    except OSError:
        return False

_CPP_COND_RE = re.compile(r'^\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)')

# The single-variable forms of #if we decide: `#if defined(X)` / `#if !defined(X)`
# (parentheses optional). Anything more complex stays opaque (kept).
_CPP_DEFINED_RE = re.compile(
    r'^\s*(!?)\s*defined\s*(?:\(\s*([A-Za-z_]\w*)\s*\)|([A-Za-z_]\w*))\s*$')


def _cpp_first_macro(rest):
    tok = rest.strip().split()
    return tok[0] if tok else ""


def _cpp_branch_cond(kind, rest):
    """Truth of an #ifdef/#ifndef/#if branch, or None when undecidable (opaque).

    #ifdef/#ifndef of a macro in CPP_DEFINED are decided; of any other macro
    they stay opaque (kept). For #if we only decide the simplest single-variable
    forms -- `#if defined(X)` and `#if !defined(X)` (parens optional): X counts
    as defined iff it is in CPP_DEFINED, everything else as undefined (false).
    Any other #if/#elif expression stays opaque, so its branch is kept."""
    if kind == "ifdef":
        return True if _cpp_first_macro(rest) in CPP_DEFINED else None
    if kind == "ifndef":
        return False if _cpp_first_macro(rest) in CPP_DEFINED else None
    if kind == "if":
        m = _CPP_DEFINED_RE.match(rest)
        if m:
            defined = (m.group(2) or m.group(3)) in CPP_DEFINED
            return not defined if m.group(1) else defined
    return None  # complex #if / any #elif: not evaluated


def active_lines(lines):
    """Yield source lines that survive preprocessing under CPP_DEFINED.

    Tracks an #if/#ifdef/#ifndef/#elif/#else/#endif stack and prunes the
    branches _cpp_branch_cond can decide: #ifdef/#ifndef of a CPP_DEFINED macro,
    and the single-variable `#if [!]defined(X)` forms (X defined iff in
    CPP_DEFINED, else false). Any other condition stays opaque and its branch is
    kept -- worst case a spurious include, never a dropped real one (except,
    deliberately, behind a `#if [!]defined(<unconfigured macro>)`)."""
    stack = []  # frames: {emit, opaque, taken, parent}

    def emitting():
        return all(fr["emit"] for fr in stack)

    for line in lines:
        m = _CPP_COND_RE.match(line)
        if not m:
            if emitting():
                yield line
            continue
        kind, rest = m.group(1), m.group(2)
        if kind in ("if", "ifdef", "ifndef"):
            parent = emitting()
            cond = _cpp_branch_cond(kind, rest)
            opaque = cond is None
            emit = parent and (True if opaque else cond)
            stack.append({"emit": emit, "opaque": opaque,
                          "taken": cond is True, "parent": parent})
        elif kind == "elif":
            if stack:
                fr = stack[-1]
                fr["opaque"] = True          # not evaluated -> keep active
                fr["emit"] = fr["parent"]
        elif kind == "else":
            if stack:
                fr = stack[-1]
                fr["emit"] = (fr["parent"] if fr["opaque"]
                              else fr["parent"] and not fr["taken"])
        elif kind == "endif":
            if stack:
                stack.pop()

# Resolver.nearest_module fallback path remaps, tried only when no ancestor of
# the original path declares a real module. `include/ydb-cpp-sdk/<rest>`
# (ydb/public/sdk/cpp's public headers) mostly has no ya.make of its own --
# the implementation + BUILD.gn for it live under the parallel `src/<rest>`.
INCLUDE_ROOT_REMAP = [
    ("ydb/public/sdk/cpp/include/ydb-cpp-sdk/", "ydb/public/sdk/cpp/src/"),
]

# Targets whose headers may be "back-included" without forming a GN dependency.
# Key = the providing target (a source-root-relative dir, e.g. "ydb/core/foo");
# value = list of consumer dirs allowed to #include the key's headers.
#
# Normal flow: if A #includes B, then A gets a dep on B (A -> B). If B *also*
# #includes A, the back-edge B -> A would close a cycle, which GN forbids. That
# back-edge is dropped only when A is a key here and B is listed in its value --
# i.e. A's headers are explicitly allowed to be included by B. The forward
# A -> B dependency is unaffected.
AllowBackwardIncludes = {
    "library/cpp/monlib/service": [
        "library/cpp/monlib/service/pages",
    ],
    "library/cpp/streams/lz": [
        "library/cpp/streams/lz/lz4",
        "library/cpp/streams/lz/snappy",
    ],
    "library/cpp/threading/future": [
        "library/cpp/threading/cancellation",
    ],
    "ydb/core/base": [
        "ydb/core/base/services",
    ],
    "ydb/library/actors/actor_type": [
        "ydb/library/actors/prof", # no real deps - single struct in common.h
    ],
    "ydb/library/actors/core": [
        "ydb/library/actors/core/events", # part of core
    ]
}


# --- ya.make parsing -------------------------------------------------------

class Module:
    def __init__(self, directory):
        self.dir = directory
        self.kind = None
        self.name = None
        self.srcs = []
        self.srcdirs = []
        self.peerdirs = []
        self.enum_headers = []
        self.macros = []
        self.transitive_headers_no = False
        self.no_util = False
        self.use_common_google_apis = False


def scan_macros(text):
    n = len(text)
    for m in MACRO_RE.finditer(text):
        name = m.group(1)
        depth, j = 0, m.end() - 1
        while j < n:
            c = text[j]
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        args = re.sub(r"#.*", "", text[m.end():j])
        yield name, args.split()


# Values for identifiers that can appear in ya.make IF/ELSEIF conditions,
# describing the configuration GN builds for (Linux/x86_64/Clang). Anything
# not listed here defaults to "" (falsy, and unequal to any non-empty
# string) -- i.e. "off"/"not this one". Extend this map as needed: a value
# of True reads as "yes" in string comparisons (e.g. `OS_LINUX == "yes"`),
# a string value is compared as-is (e.g. `BUILD_TYPE == "RELEASE"`).
CONDITION_VARS = {
    "OS_LINUX": True,
    "LINUX": True,
    "ARCH_X86_64": True,
    "CLANG": True,
    "YQL_DISABLE_YT": True,
}


def _cond_as_bool(value):
    if isinstance(value, bool):
        return value
    return value not in ("", "no")


# Maps a ya.make condition variable (see CONDITION_VARS) to the C/C++
# preprocessor macro that its enabled IF()-branch defines via
# CFLAGS(GLOBAL -D<macro>). This mirrors the compiler: the same CONDITION_VARS
# entry that selects the ya.make branch also makes the macro visible to the
# preprocessor. CPP_DEFINED below is derived from it, so CONDITION_VARS stays
# the single knob; the map only bridges the (possibly different) names.
YA_VAR_TO_CPP_DEFINE = {
    "YQL_DISABLE_YT": "YQL_DISABLE_YT",
}

# Preprocessor macros considered "defined" while scanning #include lines.
CPP_DEFINED = {
    macro for var, macro in YA_VAR_TO_CPP_DEFINE.items()
    if _cond_as_bool(CONDITION_VARS.get(var, ""))
}


def _cond_as_str(value):
    if value is True:
        return "yes"
    if value is False:
        return "no"
    return value


class CondEval:
    """Evaluates ya.make IF/ELSEIF condition expressions against a map of
    identifier -> True/False/string values (see CONDITION_VARS).

    Grammar (OR binds loosest, then AND, then NOT, then comparisons):
        expr       := or_expr
        or_expr    := and_expr (OR and_expr)*
        and_expr   := not_expr (AND not_expr)*
        not_expr   := NOT not_expr | comparison
        comparison := atom [(==|!=|>=|<=|MATCHES) atom]
        atom       := IDENTIFIER | "string" | ${IDENTIFIER}

    A bare identifier (or the left side of a comparison) is a variable
    reference: looked up in `variables`, defaulting to "" if absent. An
    unrecognized bareword on the right side of a comparison is its own
    literal text instead (e.g. `BUILD_TYPE == RELEASE` compares against the
    literal string "RELEASE", not a variable named RELEASE).
    """

    _COMPARE_OPS = {"==", "!=", ">=", "<=", "MATCHES"}
    _TOKEN_RE = re.compile(r'\$\{[^}]*\}|"[^"]*"|[A-Za-z_][A-Za-z0-9_]*|\d+(?:\.\d+)?|==|!=|>=|<=')

    def __init__(self, variables):
        self.vars = variables

    def eval(self, cond):
        self._toks = self._TOKEN_RE.findall(cond)
        self._pos = 0
        result = self._or()
        if self._pos != len(self._toks):
            raise ValueError(f"unexpected trailing tokens in condition: {cond!r}")
        return result

    def _peek(self):
        return self._toks[self._pos] if self._pos < len(self._toks) else None

    def _next(self):
        tok = self._toks[self._pos]
        self._pos += 1
        return tok

    def _or(self):
        val = self._and()
        while self._peek() == "OR":
            self._next()
            val = self._and() or val
        return val

    def _and(self):
        val = self._not()
        while self._peek() == "AND":
            self._next()
            val = self._not() and val
        return val

    def _not(self):
        if self._peek() == "NOT":
            self._next()
            return not self._not()
        return self._comparison()

    def _comparison(self):
        left = self._lookup(self._next())
        op = self._peek()
        if op not in self._COMPARE_OPS:
            return _cond_as_bool(left)
        self._next()
        right = self._literal(self._next())
        if op == "==":
            return _cond_as_str(left) == _cond_as_str(right)
        if op == "!=":
            return _cond_as_str(left) != _cond_as_str(right)
        if op == "MATCHES":
            return _cond_as_str(right) in _cond_as_str(left)
        try:
            lv, rv = int(_cond_as_str(left)), int(_cond_as_str(right))
        except ValueError:
            lv, rv = _cond_as_str(left), _cond_as_str(right)
        return lv >= rv if op == ">=" else lv <= rv

    def _lookup(self, tok):
        if tok.startswith('"'):
            return tok[1:-1]
        if tok.startswith("${"):
            return self.vars.get(tok[2:-1], "")
        return self.vars.get(tok, "")

    def _literal(self, tok):
        if tok.startswith('"'):
            return tok[1:-1]
        if tok.startswith("${"):
            return self.vars.get(tok[2:-1], "")
        return self.vars.get(tok, tok)


_COND_EVAL = CondEval(CONDITION_VARS)

_IF_RE = re.compile(r'\bIF\s*\(\s*([^()]*?)\s*\)')
_IF_CHAIN_TOKEN_RE = re.compile(r'\b(IF|ELSEIF|ELSE|ENDIF)\b\s*(?:\(\s*([^()]*?)\s*\))?')
_INCLUDE_RE = re.compile(r'\bINCLUDE\s*\(\s*([^()]*?)\s*\)')


def strip_yamake_comments(text):
    """Drop `#` comments from ya.make text before it is scanned.

    ya.make uses `#` for comments to end of line, has no block comments and no
    string literals that contain `#`, so cutting each line at its first `#` is
    safe. Line endings are preserved so IF/ELSEIF/ELSE/ENDIF chains, INCLUDE()
    and macro scanning still line up. Without this, an uppercase word followed
    by `(` or a bare IF/ELSE/ENDIF inside a comment would be misread as a real
    directive."""
    out = []
    for line in text.splitlines(keepends=True):
        body = line.rstrip("\r\n")
        eol = line[len(body):]
        out.append(body.split("#", 1)[0] + eol)
    return "".join(out)


def _extract_if_chain(text, m, cond_eval):
    """`m` matched `IF (<cond>)`. Find the matching ELSEIF/ELSE/ENDIF chain
    (tracking nested IF/ENDIF depth) and return `(body, end)`: the text of
    the first branch whose condition evaluates true (or the ELSE branch, or
    "" if none matched), and the offset right after the closing ENDIF().
    Returns None if the chain can't be resolved (unbalanced, or a condition
    CondEval doesn't understand) -- the caller then leaves it untouched."""
    branches = []
    cond, start, depth, pos = m.group(1), m.end(), 1, m.end()
    while True:
        tok = _IF_CHAIN_TOKEN_RE.search(text, pos)
        if tok is None:
            return None
        kw = tok.group(1)
        if kw == "IF":
            depth += 1
        elif kw == "ENDIF":
            depth -= 1
            if depth == 0:
                branches.append((cond, text[start:tok.start()]))
                end = tok.end()
                break
        elif depth == 1 and kw in ("ELSEIF", "ELSE"):
            branches.append((cond, text[start:tok.start()]))
            cond, start = (tok.group(2) if kw == "ELSEIF" else None), tok.end()
        pos = tok.end()
    for cond, body in branches:
        try:
            taken = cond is None or cond_eval.eval(cond)
        except (ValueError, IndexError):
            return None
        if taken:
            return body, end
    return "", end


def resolve_conditionals(text, cond_eval=_COND_EVAL):
    """Resolve IF/ELSEIF/ELSE/ENDIF chains, keeping only the body of the
    first branch whose condition evaluates true against `cond_eval` (or the
    ELSE branch, or nothing). Recurses into the kept body so nested
    conditionals are resolved too. Chains that can't be parsed are left
    untouched, surfacing later as unrecognized IF/ELSEIF/ELSE/ENDIF macros."""
    out, pos = [], 0
    while True:
        m = _IF_RE.search(text, pos)
        if not m:
            out.append(text[pos:])
            break
        out.append(text[pos:m.start()])
        result = _extract_if_chain(text, m, cond_eval)
        if result is None:
            out.append(text[m.start():m.end()])
            pos = m.end()
            continue
        body, end = result
        out.append(resolve_conditionals(body, cond_eval))
        pos = end
    return "".join(out)


def resolve_includes(text, root, directory, cond_eval=_COND_EVAL, _seen=frozenset()):
    """Inline INCLUDE(<path>) macros: each is replaced by the referenced
    file's contents, with that file's own IF/ELSEIF/ENDIF chains and nested
    INCLUDEs resolved too -- so macros defined in a .inc file are processed
    exactly as if they were written in the ya.make directly. `<path>` is
    either `${ARCADIA_ROOT}/...` (repo-root-relative) or a bare path relative
    to `directory`. Unreadable/cyclic INCLUDEs are left untouched, surfacing
    later as an unrecognized macro."""
    out, pos = [], 0
    while True:
        m = _INCLUDE_RE.search(text, pos)
        if not m:
            out.append(text[pos:])
            break
        out.append(text[pos:m.start()])
        arg = m.group(1).strip("\"'")
        if arg.startswith("${ARCADIA_ROOT}/"):
            rel = arg[len("${ARCADIA_ROOT}/"):]
        else:
            rel = os.path.normpath(os.path.join(directory, arg))
        if rel in _seen:
            out.append(text[m.start():m.end()])
        else:
            try:
                with open(os.path.join(root, rel), encoding="utf-8") as f:
                    inc_text = strip_yamake_comments(f.read())
            except OSError:
                out.append(text[m.start():m.end()])
            else:
                inc_text = resolve_conditionals(inc_text, cond_eval)
                inc_text = resolve_includes(inc_text, root, os.path.dirname(rel),
                                             cond_eval, _seen | {rel})
                out.append(inc_text)
        pos = m.end()
    return "".join(out)


def parse_yamake(path, directory, root):
    with open(path, encoding="utf-8") as f:
        text = f.read()
    text = strip_yamake_comments(text)
    text = resolve_conditionals(text)
    text = resolve_includes(text, root, directory)
    mod = Module(directory)
    for name, args in scan_macros(text):
        if name == "SET" and args and args[0] in _IGNORED_SET_VARS:
            if (args[0] == "PROTOC_TRANSITIVE_HEADERS"
                    and len(args) >= 2 and args[1].strip("\"'").lower() == "no"):
                mod.transitive_headers_no = True
            continue
        mod.macros.append(name)
        if name in KIND_MACROS:
            mod.kind = name
            mod.name = args[0] if args else os.path.basename(directory)
        elif name == "SRCS":
            mod.srcs.extend(a for a in args if a != "GLOBAL")
        elif name == "SRC":
            if args:
                mod.srcs.append(args[0])  # per-file flags (remaining args) are ignored
        elif name == "SRCDIR":
            mod.srcdirs.extend(args)
        elif name == "PEERDIR":
            mod.peerdirs.extend(args)
        elif name == "GENERATE_ENUM_SERIALIZATION":
            mod.enum_headers.extend(args)
        elif name == "NO_UTIL":
            mod.no_util = True
        elif name == "USE_COMMON_GOOGLE_APIS":
            mod.use_common_google_apis = True
    if mod.name is None:
        mod.name = os.path.basename(directory)
    return mod


def is_test(mod):
    return any(m in TEST_MACROS for m in mod.macros)


def is_real(mod):
    return any(m in ARTIFACT_MACROS for m in mod.macros) and not is_test(mod)


def _src_prefix(mod):
    """Effective source-root-relative dir for SRCS lookup.
    SRCDIR(same-as-module-dir) and missing/empty SRCDIR → mod.dir (no prefix change).
    """
    norm_self = os.path.normpath(mod.dir)
    for d in mod.srcdirs:
        d = os.path.normpath(d)
        if d and d != "." and d != norm_self:
            return d
    return mod.dir


def nontrivial_macros(mod):
    allowed = PROTO_TRIVIAL_MACROS if mod.kind == "PROTO_LIBRARY" else TRIVIAL_MACROS
    return [m for m in mod.macros if m not in allowed]


def is_trivial(mod):
    return mod.kind in KIND_MACROS and not nontrivial_macros(mod)


def module_label(d):
    # GN target name is always basename(dir); the explicit ya.make module name
    # (e.g. PROTO_LIBRARY(library-foo-bar)) is an Arcadia uniqueness artifact and
    # is dropped -- the protobuf_library/library templates compute their own
    # unique output name.
    return "//" + d


# --- external (never-generated) targets ------------------------------------

class ExternalTargets:
    """Targets that live in BUILD.gn files this generator never writes:
    keep-marked files (`yamake2gn: keep`) anywhere, plus everything under
    contrib/. Reading the target *names* such a file declares is what lets the
    three sources of truth -- generated, keep, contrib -- compose cleanly:

      * `is_external(d)` -- d's own file is authoritative; never (re)generate
        it (the old "preserved" case, now also covering contrib uniformly).
      * `declares(d, name)` -- does the external file at d define target `name`?
        Used in Pass 2 to fold a generated child into a keep/contrib parent that
        already provides it (see plan_external_collapse).
      * `label(owner)` -- the label a dependency on an *external* `owner` must
        use; a target folded into its external parent lives at //parent:name.
        Folding of a target we *generate* into a keep parent is not decided
        here -- it is a Pass-2 (geometry-aware) concern, applied via remap.
    """

    def __init__(self, root):
        self.root = root
        self._cache = {}   # dir -> frozenset(target names) | None (not external)

    def _names(self, d):
        if d not in self._cache:
            path = os.path.join(self.root, d, "BUILD.gn")
            is_ext = (d == "contrib" or d.startswith("contrib/")
                      or buildgn_is_protected(path))
            self._cache[d] = (frozenset(parse_existing_buildgn(path))
                              if is_ext else None)
        return self._cache[d]

    def is_external(self, d):
        """True if d's own BUILD.gn is one we never write (keep-marked or
        under contrib/)."""
        return self._names(d) is not None

    def declares(self, d, name):
        """True if the external file at dir `d` defines a target `name`."""
        return name in (self._names(d) or ())

    def label(self, owner):
        """Label for a dependency on an external dir `owner`: //owner when its
        own file defines the target, //parent:name when the target was folded
        into the external parent's file, else a best-effort //owner. Returns
        None when `owner` is not external at all (a target we generate -- the
        plain //owner plus the Pass-2 remap then have the final say)."""
        name = os.path.basename(owner)
        if self.declares(owner, name):
            return module_label(owner)
        if self.declares(os.path.dirname(owner), name):
            return "//%s:%s" % (os.path.dirname(owner), name)
        if self.is_external(owner) or self.is_external(os.path.dirname(owner)):
            return module_label(owner)
        return None


# --- include resolution ----------------------------------------------------

class Resolver:
    def __init__(self, root, mods):
        self.root = root
        self.mods = mods
        self._cache = {}

    def _has_yamake(self, d):
        if d not in self._cache:
            self._cache[d] = os.path.exists(os.path.join(self.root, d, "ya.make"))
        return self._cache[d]

    def _walk_up(self, d):
        # Nearest ancestor whose ya.make declares an actual buildable module
        # (LIBRARY/PROGRAM/PROTO_LIBRARY). A pure RECURSE aggregator (or
        # PY3_LIBRARY/UNITTEST/etc., kind not in KIND_MACROS) produces no GN
        # target, so it cannot own anyone's #include -- skip past it.
        # contrib/ dirs are not in `mods` (unparsed); any ya.make there is
        # treated as an owner, as before.
        while True:
            if self._has_yamake(d):
                mod = self.mods.get(d)
                if mod is None or mod.kind in KIND_MACROS:
                    return d
            if not d or d == ".":
                return None
            d = os.path.dirname(d)

    def nearest_module(self, rel_path):
        owner = self._walk_up(os.path.dirname(rel_path))
        if owner is not None:
            return owner
        for old, new in INCLUDE_ROOT_REMAP:
            if rel_path.startswith(old):
                remapped = new + rel_path[len(old):]
                owner = self._walk_up(os.path.dirname(remapped))
                if owner is not None:
                    return owner
        return None

    def resolve(self, inc, is_quote, file_dir):
        cands = []
        if is_quote:
            cands.append(os.path.normpath(os.path.join(file_dir, inc)))
        cands.append(os.path.normpath(inc))
        for rel in cands:
            if rel.startswith(".."):
                continue
            if os.path.exists(os.path.join(self.root, rel)):
                return rel
        return None


def module_files(mod, resolver):
    """Files whose includes define the module's deps:
      * compiled sources & proto sources -- EXACTLY what SRCS lists;
      * headers -- only those reachable transitively, via #include, starting
        from the module's own SRCS files. A stray foo_ut.cpp / foo_ut_common.h
        sitting in the dir but not in (or reachable from) SRCS belongs to a
        sibling ut/ module, not to us.
    """
    target_dir = mod.dir
    srcdir = _src_prefix(mod)
    seeds = [os.path.join(srcdir, s) for s in mod.srcs
             if s.endswith(SOURCE_EXTS + HEADER_EXTS + PROTO_EXTS)]
    out = list(seeds)
    seen = set(seeds)
    queue = [f for f in seeds if f.endswith(SOURCE_EXTS + HEADER_EXTS)]
    while queue:
        f = queue.pop()
        fdir = os.path.dirname(f)
        try:
            with open(os.path.join(resolver.root, f), encoding="utf-8",
                      errors="ignore") as fh:
                lines = fh.readlines()
        except OSError:
            continue
        for line in active_lines(lines):
            m = INCLUDE_RE.match(line)
            if not m:
                continue
            rel = resolver.resolve(m.group(2), m.group(1) == '"', fdir)
            if rel is None or rel in seen or not rel.endswith(HEADER_EXTS):
                continue
            if resolver.nearest_module(rel) != target_dir:
                continue
            seen.add(rel)
            out.append(rel)
            queue.append(rel)
    return out


# --- target spec -----------------------------------------------------------

class TargetSpec:
    def __init__(self, mod):
        self.dir = mod.dir
        self.name = os.path.basename(mod.dir)   # GN name = basename(dir)
        self.kind = mod.kind
        self.no_util = mod.no_util
        self.public_deps = set()
        self.deps = set()                       # all owners (filled in pass 1)
        self.header_owners = defaultdict(set)   # own header path -> owners it pulls
        self.proto_public = set()               # proto imports (always public)
        self.sources = []                       # repo-relative, non-proto
        self.proto_sources = []                 # repo-relative .proto sources
        self.enum_headers = []                  # repo-relative (GENERATE_ENUM_SERIALIZATION)
        self.host = mod.dir                      # set during merge planning


def _record(spec, rel, owner, target_dir, header_file, consumed, report, external):
    """Register one resolved include/import edge of `target_dir`."""
    if owner is None:
        # Resolved to a real file, but no ancestor declares a buildable
        # module (and no INCLUDE_ROOT_REMAP applies) -- can't derive a dep.
        report.unresolved[target_dir].add(rel)
        return
    if owner == target_dir:
        return
    if target_dir in AllowBackwardIncludes.get(owner, ()):
        # `owner`'s headers are explicitly allowed to be back-included by
        # `target_dir`; drop the would-be cyclic target_dir -> owner edge so
        # only the forward owner-as-consumer direction remains.
        return
    if owner == "util" or owner.startswith("util/"):
        return
    # A target we generate keeps the plain //owner here; if Pass 2 folds it into
    # a keep parent, the remap rewrites it then. Contrib is never generated, so
    # its (possibly folded) external label must be resolved now.
    label = module_label(owner)
    if owner.startswith("contrib/"):
        label = external.label(owner) or label
    if label in PROTO_PROVIDED_LABELS:
        return  # protobuf_library template injects protobuf/grpc itself
    if owner.startswith("contrib/"):
        report.contrib_deps[target_dir].add(label)
    spec.deps.add(label)
    # This header file is included by `target_dir` (a consumer): mark it as
    # externally consumed so its owner may have to re-export what it pulls.
    consumed.add(rel)
    if header_file is not None:
        spec.header_owners[header_file].add(label)


def gen_spec(mod, resolver, consumed, report, external):
    target_dir = mod.dir
    if mod.transitive_headers_no:
        report.transitive_headers_no.append(target_dir)
    spec = TargetSpec(mod)
    for f in module_files(mod, resolver):
        fdir = os.path.dirname(f)
        if any(s in os.path.basename(f) for s in ARCH_SUFFIXES):
            report.arch_files.append(f)
        try:
            with open(os.path.join(resolver.root, f), encoding="utf-8",
                      errors="ignore") as fh:
                lines = fh.readlines()
        except OSError:
            continue
        if f.endswith(PROTO_EXTS):
            # proto import: the generated .pb.h is the proto lib's public face,
            # so a proto->proto dependency is always public.
            for line in lines:
                m = IMPORT_RE.match(line)
                if not m:
                    continue
                rel = resolver.resolve(m.group(1), False, fdir)
                if rel is None:
                    if "/" in m.group(1):
                        report.unresolved[target_dir].add(m.group(1))
                    continue
                owner = resolver.nearest_module(rel)
                _record(spec, rel, owner, target_dir, None, consumed, report, external)
                if owner and owner != target_dir:
                    label = module_label(owner)
                    if owner.startswith("contrib/"):
                        label = external.label(owner) or label
                    if label not in PROTO_PROVIDED_LABELS:
                        spec.proto_public.add(label)
            continue
        header_file = f if f.endswith(HEADER_EXTS) else None
        for line in active_lines(lines):
            m = INCLUDE_RE.match(line)
            if not m:
                continue
            inc = m.group(2)
            rel = resolver.resolve(inc, m.group(1) == '"', fdir)
            if rel is None and PB_RE.search(inc):
                # generated proto header: map *.pb.h / *.grpc.pb.h -> *.proto
                rel = resolver.resolve(PB_RE.sub(".proto", inc), False, fdir)
            if rel is None:
                if "/" in inc:
                    report.unresolved[target_dir].add(inc)
                continue
            _record(spec, rel, resolver.nearest_module(rel), target_dir,
                    header_file, consumed, report, external)

    if mod.use_common_google_apis:
        spec.proto_public.add(GOOGLEAPIS_COMMON_PROTOS_LABEL)

    peer = {module_label(p.rstrip("/")) for p in mod.peerdirs}
    peer.discard("//util")
    report.peer_diff[target_dir] = (sorted(peer - spec.deps), sorted(spec.deps - peer))

    srcdir = _src_prefix(mod)
    spec.sources = [os.path.join(srcdir, s)
                    for s in mod.srcs if s.endswith(SOURCE_EXTS)]
    spec.proto_sources = [os.path.join(srcdir, s)
                           for s in mod.srcs if s.endswith(PROTO_EXTS)]
    spec.enum_headers = [os.path.join(srcdir, h) for h in mod.enum_headers]
    return spec


def finalize_publicity(specs, consumed):
    """A dependency is public_deps iff it is reached through one of the target's
    OWN headers that is actually included by some other generated target. Proto
    imports are always public.

    If the module itself has .proto sources and isn't a PROTO_LIBRARY, those
    sources are split out into a sibling protobuf_library("private_proto")
    (see render_spec) -- proto-import deps belong to that sub-target, not
    here."""
    for spec in specs:
        split_proto = spec.kind != "PROTO_LIBRARY" and spec.proto_sources
        pub = set() if split_proto else set(spec.proto_public)
        for header, owners in spec.header_owners.items():
            if header in consumed:
                pub |= owners
        spec.public_deps = pub
        spec.deps -= pub
        if split_proto:
            spec.deps -= spec.proto_public


# --- merge planning (Pass 2) -----------------------------------------------

def plan_merge(specs, report):
    """Pass 2 runs strictly over what Pass 1 generated: children and leaf-ness
    are computed from the generated set, not the filesystem. Modules not reached
    (skipped or outside the closure) are simply invisible here."""
    by_dir = {s.dir: s for s in specs}
    gen = set(by_dir)
    children = defaultdict(list)
    for d in gen:
        children[os.path.dirname(d)].append(d)

    def leaf(d):
        pref = d + "/"
        return not any(e.startswith(pref) for e in gen)

    remap, absorbed, dropped = {}, set(), set()
    for P, spec in by_dir.items():
        kids = children.get(P, [])
        if not kids:
            continue
        nonleaf = [os.path.basename(c) + ":non-leaf" for c in kids if not leaf(c)]
        if nonleaf:
            report.merge_blocked.append((P, nonleaf))
            continue
        taken = {spec.name}
        for c in kids:
            absorbed.add(c)
            by_dir[c].host = P
            cs = by_dir[c]
            if cs.name in taken:
                cs.name = os.path.basename(P) + "_" + cs.name
            taken.add(cs.name)
            remap[module_label(c)] = "//%s:%s" % (P, cs.name)
        if (not spec.sources and not spec.proto_sources
                and not spec.deps and not spec.public_deps
                and not spec.enum_headers):
            dropped.add(P)
        report.merged.append((P, sorted(by_dir[c].name for c in kids)))
    return remap, absorbed, dropped


def plan_external_collapse(specs, external, report):
    """Fold a generated leaf child into an *external* parent (keep-marked or
    contrib) that already declares the collapsed target.

    Pass 1 generates these children honestly -- so their #include-derived deps
    still feed the closure -- and only here, mirroring the normal merge's
    geometry, do we drop the ones the parent already provides. A child folds iff

      * it is a leaf in the generated set (same rule plan_merge uses), and
      * its external parent's file actually declares a target of the child's
        name.

    For such a child we remap //P/C -> //P:C (so dependents reach the
    hand-written/contrib target), skip emitting it, and delete its stale
    BUILD.gn. A non-leaf child, or one the parent does not declare, is left
    exactly as the normal flow produced it -- we don't even look at the parent
    file in that case."""
    by_dir = {s.dir: s for s in specs}
    gen = set(by_dir)
    children = defaultdict(list)
    for d in gen:
        children[os.path.dirname(d)].append(d)

    def leaf(d):
        pref = d + "/"
        return not any(e.startswith(pref) for e in gen)

    remap, collapsed = {}, set()
    for P, kids in children.items():
        if P in by_dir or not external.is_external(P):
            continue  # generated parent -> plan_merge; non-external -> nothing
        for c in kids:
            name = os.path.basename(c)
            if leaf(c) and external.declares(P, name):
                label = "//%s:%s" % (P, name)
                remap[module_label(c)] = label
                collapsed.add(c)
                report.keep_collapsed.append((c, label))
    return remap, collapsed


# --- rendering -------------------------------------------------------------

_TARGET_RE = re.compile(
    r'(?:library|source_set|executable|protobuf_library|group)\(\s*"([^"]+)"\s*\)\s*\{')


def _extract_list(body, key):
    m = re.search(r'(?<![\w])' + key + r'\s*=\s*\[(.*?)\]', body, re.S)
    return set(re.findall(r'"([^"]+)"', m.group(1))) if m else set()


def parse_existing_buildgn(path):
    """Existing visibility is authoritative (it may carry decisions from a wider
    run or from hand edits in GN). Return {target_name: (deps, public_deps)}."""
    try:
        with open(path, encoding="utf-8") as f:
            text = f.read()
    except OSError:
        return {}
    out = {}
    for m in _TARGET_RE.finditer(text):
        depth, j, n = 1, m.end(), len(text)
        while j < n and depth:
            depth += {"{": 1, "}": -1}.get(text[j], 0)
            j += 1
        body = text[m.end():j - 1]
        out[m.group(1)] = (_extract_list(body, "deps"),
                           _extract_list(body, "public_deps"))
    return out


def shorten(label, host):
    prefix = "//%s:" % host
    if label.startswith(prefix):
        return label[len("//" + host):]   # -> ":name"
    return label


def _render_target(tmpl, name, public_deps, deps, sources, serialize_enum_headers=()):
    lines = ['%s("%s") {' % (tmpl, name)]

    def block(key, items):
        if not items:
            return
        lines.append("    %s = [" % key)
        for it in items:
            lines.append('        "%s",' % it)
        lines.append("    ]")
        lines.append("")

    block("public_deps", public_deps)
    block("deps", deps)
    block("sources", sources)
    block("serialize_enum_headers", serialize_enum_headers)
    if lines[-1] == "":
        lines.pop()
    lines.append("}")
    return "\n".join(lines)


def render_spec(spec, host, remap, existing):
    tmpl = {"PROGRAM": "executable", "GROUP": "group",
            "PROTO_LIBRARY": "protobuf_library"}.get(spec.kind, "library")
    if tmpl == "library" and spec.no_util:
        # NO_UTIL(): build without the default //util dep -- contrib_library
        # mirrors that (platform_deps + libcxx instead of //util).
        tmpl = "contrib_library"

    # A non-PROTO_LIBRARY module with .proto sources (e.g. LIBRARY() with a
    # mix of .cpp and .proto in SRCS) can't list them directly: GN binary
    # targets only accept source/header/object files. Split the .proto
    # sources into a sibling protobuf_library("private_proto") and depend on
    # it -- strictly via deps, never public_deps.
    split_proto = tmpl != "protobuf_library" and bool(spec.proto_sources)

    pub = {shorten(remap.get(l, l), host) for l in spec.public_deps}
    dep = {shorten(remap.get(l, l), host) for l in spec.deps} - pub

    # Preserve the visibility recorded in the existing file for deps that are
    # still actual; only brand-new deps use the freshly computed classification.
    old_deps, old_pub = existing.get(spec.name, (set(), set()))
    if old_deps or old_pub:
        new_pub, new_dep = set(), set()
        for l in pub | dep:
            if l in old_pub:
                new_pub.add(l)
            elif l in old_deps:
                new_dep.add(l)
            elif l in pub:
                new_pub.add(l)
            else:
                new_dep.add(l)
        pub, dep = new_pub, new_dep - new_pub

    if split_proto:
        dep.add(":private_proto")

    pub = sorted(pub)
    dep = sorted(dep)
    main_sources = spec.sources + spec.proto_sources if tmpl == "protobuf_library" else spec.sources
    srcs = sorted(os.path.relpath(s, host) for s in main_sources)
    enums = sorted(os.path.relpath(h, host) for h in spec.enum_headers)

    # A LIBRARY() whose SRCS hold no compilable files (e.g. only a header) is a
    # facade that just re-exports its PEERDIRs. GN has no empty static library;
    # emit a group() that forwards the deps instead.
    if tmpl in ("library", "contrib_library") and not srcs and not split_proto and not enums:
        tmpl = "group"

    out = []
    if split_proto:
        proto_pub = sorted({shorten(remap.get(l, l), host) for l in spec.proto_public})
        proto_srcs = sorted(os.path.relpath(s, host) for s in spec.proto_sources)
        out.append(_render_target("protobuf_library", "private_proto", proto_pub, [], proto_srcs))
    out.append(_render_target(tmpl, spec.name, pub, dep, srcs, enums))
    return "\n\n".join(out)


def render_file(root, host, specs, remap):
    existing = parse_existing_buildgn(os.path.join(root, host, "BUILD.gn"))
    return "\n\n".join(render_spec(s, host, remap, existing)
                       for s in sorted(specs, key=lambda s: s.name)) + "\n"


# --- reporting -------------------------------------------------------------

class Report:
    def __init__(self):
        self.skipped = []
        self.unresolved = defaultdict(set)
        self.contrib_deps = defaultdict(set)
        self.arch_files = []
        self.transitive_headers_no = []
        self.peer_diff = {}
        self.merged = []
        self.merge_blocked = []
        self.keep_collapsed = []
        self.generated = []
        self.deleted = []
        self.preserved = []

    def dump(self):
        o = sys.stderr
        print("\n==================== yamake2gn report ====================", file=o)
        print("generated : %d   deleted(BUILD.gn): %d"
              % (len(self.generated), len(self.deleted)), file=o)
        if self.merged:
            print("\nmerged (parent <- children):", file=o)
            for p, kids in sorted(self.merged):
                print("  //%s <- %s" % (p, ", ".join(kids)), file=o)
        if self.merge_blocked:
            print("\nmerge blocked:", file=o)
            for p, reasons in sorted(self.merge_blocked):
                print("  //%s : %s" % (p, ", ".join(reasons)), file=o)
        if self.keep_collapsed:
            print("\nfolded into external parent (child not generated, dep -> folded label):", file=o)
            for c, label in sorted(self.keep_collapsed):
                print("  //%s -> %s" % (c, label), file=o)
        if self.skipped:
            print("\nskipped (non-trivial):", file=o)
            for d, reasons in sorted(self.skipped):
                print("  %-58s %s" % (d, ",".join(sorted(set(reasons)))), file=o)
        if self.preserved:
            print("\npreserved (yamake2gn: keep -- not regenerated):", file=o)
            for d in sorted(set(self.preserved)):
                print("  %s" % d, file=o)
        if self.arch_files:
            print("\nper-file arch sources (need -m<arch>, NOT emitted):", file=o)
            for f in sorted(set(self.arch_files)):
                print("  %s" % f, file=o)
        if self.transitive_headers_no:
            print("\nSET(PROTOC_TRANSITIVE_HEADERS \"no\") ignored (no GN equivalent;"
                  " generated .pb.h will pull in full transitive deps; any"
                  " #include of the ya.make-only \"*.deps.pb.h\" will be unresolved):",
                  file=o)
            for d in sorted(set(self.transitive_headers_no)):
                print("  %s" % d, file=o)
        total = sum(len(v) for v in self.unresolved.values())
        print("\nunresolved/generated includes: %d (in %d modules)"
              % (total, len(self.unresolved)), file=o)
        for d in sorted(self.unresolved):
            for inc in sorted(self.unresolved[d]):
                print("  [%s] %s" % (d, inc), file=o)
        print("\nPEERDIR vs derived diff:", file=o)
        for d in sorted(self.peer_diff):
            only_peer, only_derived = self.peer_diff[d]
            if only_peer or only_derived:
                print("  %s" % d, file=o)
                for l in only_peer:
                    print("      peerdir-only : %s" % l, file=o)
                for l in only_derived:
                    print("      derived-only : %s" % l, file=o)


# --- driver ----------------------------------------------------------------

def find_yamakes(root, start):
    for cur, dirs, files in os.walk(start):
        rel = os.path.relpath(cur, root)
        parts = rel.split(os.sep)
        if parts[0] == "contrib":
            dirs[:] = []
            continue
        if "ya.make" in files:
            yield rel


def find_module_dir(root, rel):
    """Nearest dir at or above `rel` that owns a ya.make."""
    d = os.path.normpath(rel)
    while d and d != ".":
        if os.path.exists(os.path.join(root, d, "ya.make")):
            return d
        d = os.path.dirname(d)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="*",
                    help="subtrees to generate; default: whole tree (non-contrib)")
    ap.add_argument("--as-target", action="store_true",
                    help="treat paths as targets: generate only the transitive "
                         "dependency closure reached from them (via #include graph)")
    ap.add_argument("--root", default=os.getcwd())
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--no-format", action="store_true")
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    if args.as_target and not args.paths:
        ap.error("--as-target requires at least one path")

    # ---- collect: parse every non-contrib ya.make (need full module graph) ---
    mods = {}
    for d in find_yamakes(root, root):
        mods[d] = parse_yamake(os.path.join(root, d, "ya.make"), d, root)
    real_dirs = {d for d, m in mods.items() if is_real(m)}

    resolver = Resolver(root, mods)
    external = ExternalTargets(root)
    report = Report()
    specs_by_dir = {}
    consumed = set()        # header files included across module boundaries

    def gen_one(d):
        if external.is_external(d):
            # Hand-maintained file (keep-marked; contrib never reaches here):
            # authoritative, so leave it untouched. Returning None keeps it out
            # of the spec set -- never rewritten, absorbed, nor deleted; in
            # --as-target mode its existing deps are still followed below.
            #
            # A target that this file *folds* from a child dir is handled in
            # Pass 2 (plan_external_collapse): the child is still generated here
            # so its deps feed the closure, then dropped only if geometry allows.
            report.preserved.append(d)
            return None
        mod = mods[d]
        if not is_trivial(mod):
            report.skipped.append((d, nontrivial_macros(mod) or ["<no module>"]))
            return None
        s = gen_spec(mod, resolver, consumed, report, external)
        specs_by_dir[d] = s
        return s

    if args.as_target:
        # ---- Pass 1: transitive closure over the derived #include graph -------
        queue = []
        for p in args.paths:
            md = find_module_dir(root, os.path.relpath(os.path.abspath(p), root))
            if md is None:
                ap.error("no ya.make at or above %s" % p)
            queue.append(md)
        seen = set()
        while queue:
            d = queue.pop()
            if d in seen or d not in mods or not is_real(mods[d]):
                seen.add(d)
                continue
            seen.add(d)
            s = gen_one(d)
            if s is None:
                # Non-trivial module skipped by generator: if a hand-written
                # BUILD.gn already exists, follow its deps so that their
                # targets are generated too.
                existing_gn = os.path.join(root, d, "BUILD.gn")
                if os.path.exists(existing_gn):
                    existing = parse_existing_buildgn(existing_gn)
                    for _deps, _pub in existing.values():
                        for label in _deps | _pub:
                            dep = label[2:].split(":")[0]
                            if not dep.startswith("contrib/"):
                                queue.append(dep)
                continue
            for label in s.deps | s.public_deps:
                dep = label[2:].split(":")[0]   # //dir or //dir:name -> dir
                if dep.startswith("contrib/"):
                    continue                    # external: keeps its own BUILD.gn
                queue.append(dep)
    else:
        # ---- Pass 1: whole tree or selected subtrees --------------------------
        if args.paths:
            roots = [os.path.relpath(os.path.abspath(p), root) for p in args.paths]
            sel = [d for d in real_dirs
                   if any(d == r or d.startswith(r + "/") for r in roots)]
        else:
            sel = list(real_dirs)
        for d in sorted(sel):
            gen_one(d)

    specs = list(specs_by_dir.values())

    # publicity is a graph property: resolve it once the consumer set is known
    finalize_publicity(specs, consumed)

    # ---- Pass 2: merge, strictly over what Pass 1 generated ------------------
    remap, absorbed, dropped = plan_merge(specs, report)

    # Generated leaf children whose collapsed target a keep/contrib parent
    # already provides: drop them and remap //child -> //parent:child. Decided
    # here (not Pass 1) so the children were generated honestly and only the
    # geometry that permits a normal merge permits this fold too.
    ext_remap, collapsed = plan_external_collapse(specs, external, report)
    remap.update(ext_remap)

    # Pure aggregators that were NOT merged: keep the bundle as a group() of its
    # direct real children (their PEERDIR-style semantics), instead of an empty
    # library.
    children_map = defaultdict(list)
    for d in real_dirs:
        children_map[os.path.dirname(d)].append(d)
    for s in specs:
        if (s.dir not in dropped and not s.sources and not s.deps
                and not s.public_deps and not s.enum_headers
                and children_map.get(s.dir)):
            s.kind = "GROUP"
            s.public_deps = {module_label(c) for c in children_map[s.dir]}

    host_specs = defaultdict(list)
    for s in specs:
        if s.dir in dropped and s.host == s.dir:
            continue  # aggregator target dropped
        if s.dir in collapsed:
            continue  # folded into a keep/contrib parent; provided there
        host_specs[s.host].append(s)

    # ---- emit ----------------------------------------------------------------
    for host, slist in sorted(host_specs.items()):
        text = render_file(root, host, slist, remap)
        out = os.path.join(root, host, "BUILD.gn")
        report.generated.append(host)
        if args.dry_run:
            print("# ----- %s/BUILD.gn -----\n%s" % (host, text))
        else:
            with open(out, "w", encoding="utf-8") as f:
                f.write(text)
            if not args.no_format:
                subprocess.run(["gn", "format", out], cwd=root, check=False,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    for c in sorted(absorbed | collapsed):
        old = os.path.join(root, c, "BUILD.gn")
        if os.path.exists(old):
            report.deleted.append(c)
            if args.dry_run:
                print("# DELETE %s/BUILD.gn (merged into %s)" % (c, os.path.dirname(c)))
            else:
                os.remove(old)

    report.dump()


if __name__ == "__main__":
    main()
