#!/usr/bin/env python3
"""Convert trivial ya.make modules to BUILD.gn (v1 + merge pass).

Scope:
  * Only modules OUTSIDE contrib/ are *generated* (contrib may still be a dep).
  * Only "trivial" modules are converted: ya.make using nothing beyond
    LIBRARY / PROGRAM / SRCS / PEERDIR / RECURSE / RECURSE_FOR_TESTS / END.
    Anything else (CFLAGS, YQL_LAST_ABI_VERSION, GENERATE_ENUM_SERIALIZATION,
    PROTO_LIBRARY, IF/ELSE, ADDINCL, tests, ...) -> SKIPPED and reported.
  * Headers dropped from `sources`.
  * deps derived from the real #include graph of the module's own files; a
    PEERDIR no #include leads to is added too, marked "# peerdir only".
  * Classification: every include -> `deps`; if it appears in one of THIS
    module's *headers*, the module re-exports it -> promote to `public_deps`.
  * `util` (and util/**) is always-available and never emitted as a dependency.
  * `# gn:` comments in ya.make split a module into several GN targets (to
    break GN dep cycles ya.make doesn't have); see GN_DIRECTIVES below.
  * RESOURCE / RESOURCE_FILES / ALL_RESOURCE_FILES[_FROM_DIRS] -> `resources`
    and `resource_files` of library() (see build/gn/resources.gni); a PROGRAM
    gets them through a sibling library("<name>_resources").

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
    "RAGEL6_FLAGS",  # TODO: the code style only (-CG1 ...), see //build/gn/source_kinds.gni
}
TRIVIAL_MACROS = MODULE_MACROS | {
    "SRCS", "SRC", "PEERDIR", "RECURSE", "RECURSE_FOR_TESTS", "RECURSE_ROOT_RELATIVE", "END", "SUBSCRIBER",
    "GENERATE_ENUM_SERIALIZATION",   # -> serialize_enum_headers
    "SRCDIR", "ADDINCL", "NO_WSHADOW", "ENABLE", "NO_COMPILER_WARNINGS", "SUPPRESSIONS", "NEED_CHECK", "ENV",
    "YQL_LAST_ABI_VERSION",   # the default UDF ABI, see //build/gn/config:yql_abi_current
    "GENERATE_ENUM_SERIALIZATION_WITH_HEADER", # TODO: temporary ignore
    "RESOURCE", "RESOURCE_FILES", "ALL_RESOURCE_FILES", "ALL_RESOURCE_FILES_FROM_DIRS",  # -> resources
    "CFLAGS", "CXXFLAGS", "ALLOCATOR_IMPL", "GRPC", # TODO: temporary ignore
    "YQL_ABI_VERSION",   # -> yql_abi_version
    "CHECK_DEPENDENT_DIRS", # TODO: temporary ignore
    "PROVIDES",  # -> link_slot_provides
    "NO_UTIL",   # LIBRARY -> contrib_library (no default //util dep)
    "GN_DIRECTIVES",   # carrier of standalone `# gn:` lines, see GN_DIRECTIVES
}
# PROTO_LIBRARY macros that are pure noise in GN (template always does grpc +
# --fatal_warnings, python/metadata/tags are irrelevant) -> drop, do not block.
PROTO_IGNORE_MACROS = {
    "PROTOC_FATAL_WARNINGS", "GRPC", "EXCLUDE_TAGS", "INCLUDE_TAGS", "ONLY_TAGS",
    "PY_NAMESPACE", "NO_OPTIMIZE_PY_PROTOS", "NO_MYPY", "LICENSE", "LICENSE_TEXTS",
    "WITHOUT_LICENSE_TEXTS", "VERSION", "SUBSCRIBER", "MAVEN_GROUP_ID",
    "ORIGINAL_SOURCE", "NO_COMPILER_WARNINGS", "ADDINCL", "YQL_LAST_ABI_VERSION",
    "CPP_PROTO_PLUGIN0", "CPP_PROTO_PLUGIN",   # -> extra_plugins
}
PROTO_TRIVIAL_MACROS = {
    "PROTO_LIBRARY", "SRCS", "PEERDIR", "RECURSE", "RECURSE_FOR_TESTS", "END",
    "GENERATE_ENUM_SERIALIZATION",   # -> serialize_enum_headers (on generated .pb.h)
    "USE_COMMON_GOOGLE_APIS",   # -> public dep on googleapis-common-protos
    "GN_DIRECTIVES",
} | PROTO_IGNORE_MACROS
# Proto-provided deps that the GN protobuf_library template already injects:
# dropped for a PROTO_LIBRARY and for .proto imports only -- any other target
# that includes <google/protobuf/...> or <grpcpp/...> depends on them as usual.
PROTO_PROVIDED_LABELS = {"//contrib/libs/protobuf", "//contrib/libs/grpc"}
# USE_COMMON_GOOGLE_APIS(...) pulls in google/api/*.proto etc., always imported
# (and thus re-exported) by the module's own .proto sources -> always public.
GOOGLEAPIS_COMMON_PROTOS_LABEL = "//contrib/libs/googleapis-common-protos"
# Its headers need YQL_LAST_ABI_VERSION() / YQL_ABI_VERSION() in the includer's
# ya.make (udf_version.h #errors otherwise); a module without either is reported.
UDF_MODULE = "yql/essentials/public/udf"
# Macros that declare an artifact-producing (non-test) module.
ARTIFACT_MACROS = KIND_MACROS | {
    "PY3_LIBRARY", "PY23_LIBRARY", "PY3_PROGRAM",
    "GO_LIBRARY", "GO_PROGRAM", "RESOURCES_LIBRARY", "DLL",
}
TEST_MACROS = {
    "UNITTEST", "UNITTEST_FOR", "GTEST", "G_BENCHMARK", "Y_BENCHMARK",
    "PY2TEST", "PY3TEST", "PY23_TEST", "BOOSTTEST", "EXECTEST", "FUZZ",
}

HEADER_EXTS = (".h", ".hpp", ".hh", ".hxx", ".inc", ".ipp", ".cuh")
SOURCE_EXTS = (".cpp", ".cc", ".cxx", ".c")
# translated to C++ by library() / contrib_library() themselves, see
# //build/gn/source_kinds.gni: listed in `sources` as ya.make lists them
TRANSLATED_EXTS = (".rl6", ".y", ".ypp")
COMPILED_EXTS = SOURCE_EXTS + TRANSLATED_EXTS
PROTO_EXTS = (".proto",)
ARCH_SUFFIXES = ("_avx", "_avx2", "_avx512", "_pclmul")

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

# Include roots that contrib publishes to everyone via `ADDINCL(GLOBAL <dir>)`,
# tried by Resolver.resolve as a last resort. Without them `#include <openssl/
# sha.h>` resolves to no file at all -- the header lives in
# contrib/libs/openssl/include/openssl/sha.h, not at the repo root -- so the
# include is merely reported unresolved and the dependency on
# //contrib/libs/openssl is silently lost. Same story for grpc, poco, arrow,
# aws, boost, ...
#
# This is deliberately a hand-kept list, not a scan of contrib's ya.make files:
# the first root that holds the header wins, so the set has to stay small,
# ordered and reviewable. Note what is NOT here -- contrib/libs/cxxsupp/libcxx/
# include is a GLOBAL ADDINCL too, and adding it would resolve `#include
# <vector>` into contrib and give every single module a dependency on libcxx;
# the same goes for linux-headers, libc_compat and the Python headers.
#
# To extend: an include that no root covers shows up in the report's
# "unresolved" section -- find the ADDINCL(GLOBAL ...) that publishes it
#     grep -rn "GLOBAL contrib/" --include=ya.make contrib/
# and add the directory below, keeping the list sorted.
CONTRIB_INCLUDE_ROOTS = [
    "contrib/libs/apache/arrow/cpp/src",                   # <arrow/api.h>
    "contrib/libs/apache/arrow/src",                       # <arrow/util/config.h>
    "contrib/libs/aws-sdk-cpp/aws-cpp-sdk-core/include",   # <aws/core/Aws.h>
    "contrib/libs/aws-sdk-cpp/aws-cpp-sdk-s3/include",     # <aws/s3/S3Client.h>
    "contrib/libs/aws-sdk-cpp/aws-cpp-sdk-sqs/include",    # <aws/sqs/SQSClient.h>
    "contrib/libs/brotli/c/include",                       # <brotli/encode.h>
    "contrib/libs/c-ares/include",                         # <ares.h>
    "contrib/libs/cctz/include",                           # <cctz/time_zone.h>
    "contrib/libs/curl/include",                           # <curl/curl.h>
    "contrib/libs/double-conversion",                      # <double-conversion/double-conversion.h>
    "contrib/libs/fmt/include",                            # <fmt/format.h>
    "contrib/libs/ftxui/include",                          # <ftxui/dom/node.hpp>
    "contrib/libs/grpc/include",                           # <grpcpp/alarm.h>
    "contrib/libs/icu/include",                            # <unicode/ucnv.h>
    "contrib/libs/jinja2cpp/include",                      # <jinja2cpp/value.h>
    "contrib/libs/libaio/include",                         # <libaio.h>
    "contrib/libs/libiconv/include",                       # <iconv.h>
    "contrib/libs/libidn/include",                         # <idna.h>
    "contrib/libs/liburing/src/include",                   # <liburing.h>
    "contrib/libs/libxml/include",                         # <libxml/uri.h>
    "contrib/libs/llvm16/include",                         # <llvm-c/Core.h>
    "contrib/libs/openldap/include",                       # <ldap.h>
    "contrib/libs/openssl/include",                        # <openssl/bn.h>
    "contrib/libs/opentelemetry-cpp/exporters/otlp/include", # <opentelemetry/exporters/otlp/...>
    "contrib/libs/opentelemetry-cpp/sdk/include",          # <opentelemetry/sdk/resource/resource.h>
    "contrib/libs/poco/Foundation/include",                # <Poco/URI.h>
    "contrib/libs/poco/JSON/include",                      # <Poco/JSON/JSON.h>
    "contrib/libs/poco/Net/include",                       # <Poco/Net/DNS.h>
    "contrib/libs/poco/NetSSL_OpenSSL/include",            # <Poco/Net/NetSSL.h>
    "contrib/libs/poco/Util/include",                      # <Poco/Util/Application.h>
    "contrib/libs/protobuf/src",                           # <google/protobuf/any.h>
    "contrib/libs/protoc/src",                             # <google/protobuf/compiler/plugin.h>
    "contrib/libs/re2/include",                            # <re2/re2.h>
    "contrib/libs/tcmalloc",                               # <tcmalloc/common.h>
    "contrib/libs/yaml-cpp/include",                       # <yaml-cpp/yaml.h>
    "contrib/libs/zlib/include",                           # <zlib.h>
    "contrib/restricted/abseil-cpp",                       # <absl/base/internal/spinlock.h>
    "contrib/restricted/boost/algorithm/include",          # <boost/algorithm/string.hpp>
    "contrib/restricted/boost/container_hash/include",     # <boost/container_hash/hash_fwd.hpp>
    "contrib/restricted/boost/core/include",               # <boost/noncopyable.hpp>
    "contrib/restricted/boost/detail/include",             # <boost/blank.hpp>
    "contrib/restricted/boost/iterator/include",           # <boost/iterator/transform_iterator.hpp>
    "contrib/restricted/boost/multi_index/include",        # <boost/multi_index/member.hpp>
    "contrib/restricted/boost/program_options/include",    # <boost/program_options/options_description.hpp>
    "contrib/restricted/boost/range/include",              # <boost/range/adaptor/map.hpp>
    "contrib/restricted/boost/smart_ptr/include",          # <boost/smart_ptr/intrusive_ptr.hpp>
    "contrib/restricted/cityhash-1.0.2",                   # <city.h>
    "contrib/restricted/dragonbox/include",                # <dragonbox/dragonbox_to_chars.h>
    "contrib/restricted/google/benchmark/include",         # <benchmark/benchmark.h>
    "contrib/restricted/googletest/googlemock/include",    # <gmock/gmock.h>
    "contrib/restricted/googletest/googletest/include",    # <gtest/gtest.h>
    "contrib/restricted/nlohmann_json/include",            # <nlohmann/json.hpp>
]

# Resolver.nearest_module fallback path remaps, tried only when no ancestor of
# the original path declares a real module. `include/ydb-cpp-sdk/<rest>`
# (ydb/public/sdk/cpp's public headers) mostly has no ya.make of its own --
# the implementation + BUILD.gn for it live under the parallel `src/<rest>`.
INCLUDE_ROOT_REMAP = [
    ("ydb/public/sdk/cpp/include/ydb-cpp-sdk/", "ydb/public/sdk/cpp/src/"),
    # brotli's public headers: an ADDINCL(GLOBAL) of c/common, which owns them
    ("contrib/libs/brotli/c/include/", "contrib/libs/brotli/c/common/"),
]
# The same remap applies to a module that does live under `include/...` (e.g.
# include/ydb-cpp-sdk/client/topic: its headers + GENERATE_ENUM_SERIALIZATION,
# PEERDIRed by src/client/topic) when the remapped dir is a module too: the two
# are one GN target, the src one. With no module at the remapped dir, the
# nearest src module above it gets it as a GN_DIRECTIVES part named after it
# (include/.../client/iam/common -> //.../src/client/iam:common). See
# Resolver.aliases / merge_aliases.

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
        self.resource_macros = []    # (macro, raw args) of RESOURCE* in file order
        self.global_srcs = False     # SRCS(GLOBAL ...): linked whenever PEERDIRed, see gen_spec
        self.resources = []          # converted, see convert_resources
        self.resources_missing = []  # input paths found nowhere in the source tree
        self.src_parts = {}          # SRCS entry (as written) -> part name, `# gn: <part>`
        self.part_headers = defaultdict(list)  # part -> headers (as written), `# gn: <part> headers ...`
        self.peerdir_parts = defaultdict(list)  # PEERDIR dir -> parts of it, `# gn: :<part> ...`
        self.gn_peerdirs = []        # (own part or None, dir, its part or None), `# gn: [<part>] peerdir ...`
        self.provides = []           # PROVIDES() names
        self.slot_headers = defaultdict(list)  # slot -> its interface headers (as written), `# gn: slot ...`
        self.anti_cycle_facade = False  # `# gn: anti-cycle facade`, see ANTI_CYCLE_FACADE
        self.yql_abi = None          # "current" (YQL_LAST_ABI_VERSION) or "M.m.p" (YQL_ABI_VERSION)
        self.generated = []          # repo-relative files the module generates (RUN_PROGRAM ... OUT, ...)
        self.proto_plugins = []      # (name, dir) of CPP_PROTO_PLUGIN0(<name> <dir>)
        self.src_into = {}           # SRCS entry (as written) -> (module dir, part or None), `# gn: into ...`
        self.foreign_srcs = {}       # another module's file compiled here -> own part or None, see link_foreign_sources
        self.absorbs = set()         # modules all of whose sources are compiled here
        self.absorbed_into = None    # label of the target that absorbed this module, see link_foreign_sources
        self.part_peerdirs = defaultdict(list)  # part -> PEERDIRs of an include-side module made that part
        self._part_of = None         # repo-relative file -> part, see part_of()

    def parts(self):
        return sorted(set(self.src_parts.values()) | set(self.part_headers)
                      | {p for p in self.foreign_srcs.values() if p is not None})

    def part_of(self, rel):
        """The part a repo-relative source/header of this module belongs to,
        or None for the module's main target."""
        if self._part_of is None:
            self._part_of = {}
            prefix = _src_prefix(self)
            for src, part in self.src_parts.items():
                self._part_of[os.path.normpath(os.path.join(prefix, src))] = part
            for part, headers in self.part_headers.items():
                for h in headers:
                    self._part_of[os.path.normpath(os.path.join(prefix, h))] = part
            self._part_of.update(self.foreign_srcs)
        return self._part_of.get(rel)

    def compiled_srcs(self):
        """SRCS entries compiled by this module's own targets (not `into` another)."""
        return [s for s in self.srcs if s not in self.src_into]


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
        yield name, args.split(), args


# Values for identifiers that can appear in ya.make IF/ELSEIF conditions,
# describing the configuration GN builds for (Linux/x86_64/Clang). Anything
# not listed here defaults to "" (falsy, and unequal to any non-empty
# string) -- i.e. "off"/"not this one". Extend this map as needed: a value
# of True reads as "yes" in string comparisons (e.g. `OS_LINUX == "yes"`),
# a string value is compared as-is (e.g. `BUILD_TYPE == "RELEASE"`).
CONDITION_VARS = {
    "ARCH_X86_64": True,
    "CLANG": True,
    "LINUX": True,
    "OPENSOURCE": True,
    "OS_LINUX": True,
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
# A PROTO_LIBRARY is a multimodule in ya make; GN builds its C++ part, whose
# MODULE_TAG is CPP_PROTO (IF (MODULE_TAG == "CPP_PROTO") in its ya.make).
_PROTO_COND_EVAL = CondEval(dict(CONDITION_VARS, MODULE_TAG="CPP_PROTO"))
_PROTO_LIBRARY_RE = re.compile(r"^\s*PROTO_LIBRARY\s*\(", re.M)

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
        code, sep, comment = body.partition("#")
        out.append(code + (_gn_directive_tokens(code, comment) if sep else "") + eol)
    return "".join(out)


# GN_DIRECTIVES: `# gn:` comments in ya.make, invisible to ya, that shape the
# GN targets of a module. ya.make has one module per directory; GN sometimes
# needs a few targets there to break a dep cycle (the ya link is whole-program,
# the GN one is per shared library).
#
#   SRCS(
#       group_mapper.cpp        # gn: group_mapper
#       bsc.cpp
#   )
#   # gn: group_mapper headers group_mapper.h group_layout_checker.h
#
#     A source marked `# gn: <part>` goes to library("<part>") instead of the
#     module's main target; `headers` assigns the module's headers to it (paths
#     as in SRCS), so an #include of them -- from another module or from the
#     rest of this one -- becomes a dep on //module:<part>. The main target
#     always depends on its parts: in ya they are simply part of the module.
#
#   PEERDIR(
#       ydb/core/tx/datashard   # gn: :read_events :range_ops
#   )
#
#     The PEERDIR means these parts of the module, not its main target.
#
#   # gn: peerdir ydb/core/mind:tenant_node_enumeration
#   # gn: worker_common peerdir ydb/core/kqp/session_actor:response
#
#     A dep GN needs and ya.make can't say: ya links the whole program, so a
#     link-only use of a part (no #include of its headers, no PEERDIR on its
#     module) is fine there. Standalone line; with a leading <part> the dep is
#     that part's, otherwise the main target's; ":<part>" may be omitted.
#
#   SRCS(
#       malloc.cpp
#       malloc.h                # gn: slot allocator
#   )
#
#     The module is the interface of a link slot (build/gn/link_slots.gni):
#     the functions the headers so marked declare, and the module itself does
#     not define, are implemented by the slot's providers. Its dependents get
#     weak references to them, see link_slot_weak_refs in
#     //build/gn/link_slots.gni. Like PROVIDES, a module is the interface of one
#     slot at most.
#
#   SRCS(
#       column.cpp              # gn: into ydb/core/tx/columnshard/engines/portions
#   )
#
#     The source is compiled by the target of another module (its main one,
#     or ":<part>"), not by this one: the other module needs the code while
#     this one depends on it. Its includes count as the other module's; if
#     all of this module's sources go there, the module is absorbed: it has
#     no target (nor BUILD.gn) of its own, its enum serialization and
#     resources move along,
#     and the other module no longer depends on it (its headers are then that
#     module's code).
#
# The comment is rewritten here, before comments are dropped, into "@gn..."
# tokens that ride along with the macro arguments (parse_yamake picks them up):
# a trailing directive becomes "@gn:<word>" tokens right after the entry it
# follows; a standalone headers line becomes GN_DIRECTIVES(@gn-headers:...).
_GN_DIRECTIVE_RE = re.compile(r"^\s*gn:\s*(.*?)\s*$")


def _gn_directive_tokens(code, comment):
    m = _GN_DIRECTIVE_RE.match(comment)
    if not m or not m.group(1):
        return ""
    words = m.group(1).split()
    token = None
    if " ".join(words) == ANTI_CYCLE_FACADE and not code.strip():
        return "GN_DIRECTIVES(@gn-anti-cycle-facade)"
    if len(words) == 2 and words[0] == "slot":
        if not code.strip():
            return ""   # a trailing directive needs an entry on its line
        return " @gn-slot:" + words[1]
    if len(words) >= 2 and words[1] == "headers":
        token = "@gn-headers:%s:%s" % (words[0], ",".join(words[2:]))
    elif words[0] == "peerdir":
        token = "@gn-peerdir::%s" % ",".join(words[1:])
    elif len(words) >= 2 and words[1] == "peerdir":
        token = "@gn-peerdir:%s:%s" % (words[0], ",".join(words[2:]))
    elif words[0] == "into":
        if not code.strip():
            return ""   # a trailing directive needs an entry on its line
        return " @gn-into:" + ",".join(words[1:])
    if token is not None:
        return (" " + token) if code.strip() else "GN_DIRECTIVES(%s)" % token
    if not code.strip():
        return ""   # a trailing directive needs an entry on its line
    return "".join(" @gn:" + w for w in words)


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
    cond_eval = _PROTO_COND_EVAL if _PROTO_LIBRARY_RE.search(text) else _COND_EVAL
    text = resolve_conditionals(text, cond_eval)
    text = resolve_includes(text, root, directory, cond_eval)
    mod = Module(directory)
    for name, args, raw in scan_macros(text):
        args = _take_gn_directives(mod, name, args)
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
            mod.global_srcs |= "GLOBAL" in args
        elif name == "SRC":
            if args:
                mod.srcs.append(args[0])  # per-file flags (remaining args) are ignored
        elif name == "SRCDIR":
            mod.srcdirs.extend(args)
        elif name == "PEERDIR":
            mod.peerdirs.extend(args)
        elif name == "GENERATE_ENUM_SERIALIZATION" or name == "GENERATE_ENUM_SERIALIZATION_WITH_HEADER": # TODO: improve WITH_HEADER macros
            mod.enum_headers.extend(args)
        elif name == "NO_UTIL":
            mod.no_util = True
        elif name == "USE_COMMON_GOOGLE_APIS":
            mod.use_common_google_apis = True
        elif name in ("CPP_PROTO_PLUGIN0", "CPP_PROTO_PLUGIN"):
            if len(args) >= 2:
                mod.proto_plugins.append((args[0], os.path.normpath(args[1])))
            else:
                mod.macros.append("GN<bad %s %s>" % (name, " ".join(args)))
        elif name == "PROVIDES":
            mod.provides.extend(args)
        elif name == "ALLOCATOR_IMPL":
            mod.provides.append(ALLOCATOR_SLOT)
        elif name in GENERATOR_MACROS:
            mod.generated.extend(_generated_outputs(name, args, directory))
        elif name == "YQL_LAST_ABI_VERSION":
            mod.yql_abi = "current"
        elif name == "YQL_ABI_VERSION":
            if len(args) == 3 and all(a.isdigit() for a in args):
                mod.yql_abi = ".".join(args)
            else:
                mod.macros.append("GN<bad YQL_ABI_VERSION %s>" % " ".join(args))
        elif name in RESOURCE_MACROS:
            mod.resource_macros.append((name, raw))
    if mod.name is None:
        mod.name = os.path.basename(directory)
    _check_gn_parts(mod)
    convert_resources(mod, root)
    return mod


_GN_PART_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


# Macros that generate files of the module (in its build dir) -- an #include
# of one resolves to no source file, yet it is the module's header: see
# Resolver.generated.
GENERATOR_MACROS = {"RUN_PROGRAM", "RUN_PYTHON3", "RUN_PY3_PROGRAM", "CONFIGURE_FILE",
                    "RUN_ANTLR", "RUN_ANTLR4", "RUN_ANTLR4_CPP"}
_GENERATOR_OUT_KEYWORDS = {"OUT", "OUT_NOAUTO", "STDOUT", "STDOUT_NOAUTO"}
_GENERATOR_KEYWORD_RE = re.compile(r"^[A-Z][A-Z0-9_]*$")


def _generated_outputs(name, args, directory):
    """Repo-relative paths of the files a generator macro writes: the OUT /
    STDOUT arguments of RUN_* (up to the next keyword), the second argument of
    CONFIGURE_FILE. Relative to the module's build dir, i.e. to its dir."""
    if name == "CONFIGURE_FILE":
        outs = args[1:2]
    else:
        outs, taking = [], False
        for a in args:
            if _GENERATOR_KEYWORD_RE.match(a):
                taking = a in _GENERATOR_OUT_KEYWORDS
            elif taking:
                outs.append(a)
    result = []
    for o in outs:
        for prefix, base in (("${BINDIR}/", directory), ("${ARCADIA_BUILD_ROOT}/", "")):
            if o.startswith(prefix):
                o = os.path.join(base, o[len(prefix):])
                break
        else:
            o = os.path.join(directory, o)
        if "${" not in o:
            result.append(os.path.normpath(o))
    return result


def _take_gn_directives(mod, macro, args):
    """Strip the "@gn..." tokens of GN_DIRECTIVES from a macro's arguments,
    recording what they say in `mod`; returns the plain arguments."""
    plain, last = [], None
    for a in args:
        if a.startswith("@gn-headers:"):
            part, _, headers = a[len("@gn-headers:"):].partition(":")
            mod.part_headers[part].extend(h for h in headers.split(",") if h)
        elif a.startswith("@gn-peerdir:"):
            part, _, targets = a[len("@gn-peerdir:"):].partition(":")
            for t in filter(None, targets.split(",")):
                d, _, sub = t.partition(":")
                mod.gn_peerdirs.append((part or None, os.path.normpath(d.rstrip("/")), sub or None))
        elif a.startswith("@gn-slot:"):
            slot = a[len("@gn-slot:"):]
            if last is None or macro not in ("SRCS", "SRC") or not last.endswith(HEADER_EXTS):
                mod.macros.append("GN<slot %r not on a header in SRCS>" % slot)
            else:
                mod.slot_headers[slot].append(last)
        elif a == "@gn-anti-cycle-facade":
            mod.anti_cycle_facade = True
        elif a.startswith("@gn-into:"):
            value = a[len("@gn-into:"):]
            d, _, sub = value.partition(":")
            if last is None or macro not in ("SRCS", "SRC") or not d or "," in value:
                mod.macros.append("GN<bad into %r in %s>" % (value, macro))
            else:
                mod.src_into[last] = (os.path.normpath(d.rstrip("/")), sub or None)
        elif a.startswith("@gn:"):
            value = a[len("@gn:"):]
            if last is None:
                mod.macros.append("GN<%s: no entry before directive>" % macro)
            elif macro in ("SRCS", "SRC"):
                mod.src_parts[last] = value.lstrip(":")
            elif macro == "PEERDIR":
                mod.peerdir_parts[os.path.normpath(last.rstrip("/"))].append(value.lstrip(":"))
            else:
                mod.macros.append("GN<directive in %s>" % macro)
        else:
            plain.append(a)
            if a != "GLOBAL":
                last = a
    return plain


def _check_gn_parts(mod):
    """Record a broken GN_DIRECTIVES setup as a non-trivial pseudo-macro, so the
    module is skipped with a reason instead of being generated wrong."""
    for part in mod.parts():
        if not _GN_PART_RE.match(part) or part == os.path.basename(mod.dir):
            mod.macros.append("GN<bad part name %r>" % part)
    for src in mod.src_parts:
        if src not in mod.srcs:
            mod.macros.append("GN<%s is not in SRCS>" % src)
        if src in mod.src_into:
            mod.macros.append("GN<%s is both a part and into another module>" % src)
    for part, _, _ in mod.gn_peerdirs:
        if part is not None and part not in mod.parts():
            mod.macros.append("GN<peerdir of unknown part %r>" % part)
    # like PROVIDES: a target is (the interface of) one slot -- the module's
    # own or one of its parts (`# gn: <part> headers ...`)
    header_part = {h: p for p, hs in mod.part_headers.items() for h in hs}
    owners = defaultdict(set)
    for slot, hs in mod.slot_headers.items():
        slot_owners = {header_part.get(h) for h in hs}
        if len(slot_owners) == 1:
            owners[slot_owners.pop()].add(slot)
        else:
            owners[None].add(slot)
    for slots in owners.values():
        if len(slots) > 1:
            mod.macros.append("GN<headers of several slots: %s>" % ", ".join(sorted(slots)))


# --- resources ---------------------------------------------------------------

RESOURCE_MACROS = ("RESOURCE", "RESOURCE_FILES", "ALL_RESOURCE_FILES", "ALL_RESOURCE_FILES_FROM_DIRS")

# RESOURCE() flags that only steer ymake's own input parsing.
_RESOURCE_FLAGS = {"DONT_PARSE", "FORCE_TEXT", "DONT_COMPRESS"}

_RESOURCE_TOKEN_RE = re.compile(r'"[^"]*"|\S+')


class UnsupportedResource(Exception):
    pass


def _resource_tokens(raw):
    return [t[1:-1] if t.startswith('"') else t for t in _RESOURCE_TOKEN_RE.findall(raw)]


def _expand_resource_vars(value, mod):
    value = value.replace("${MODDIR}", mod.dir)
    if "${" in value:
        raise UnsupportedResource(value)
    return value


def _resource_input(mod, root, path):
    """Resolve a RESOURCE input the way ymake does. Returns (ref, rootrel):
    ref is ("src", repo-relative path) or ("gen", root_gen_dir-relative path),
    rootrel is what ${rootrel:...} expands to."""
    for var, kind, base in (("${ARCADIA_ROOT}/", "src", ""),
                            ("${CURDIR}/", "src", mod.dir),
                            ("${ARCADIA_BUILD_ROOT}/", "gen", ""),
                            ("${BINDIR}/", "gen", mod.dir)):
        if path.startswith(var):
            rel = os.path.normpath(os.path.join(base, _expand_resource_vars(path[len(var):], mod)))
            return (kind, rel), rel
    path = _expand_resource_vars(path, mod)
    for base in (mod.dir, _src_prefix(mod), ""):
        rel = os.path.normpath(os.path.join(base, path))
        if not rel.startswith("..") and os.path.exists(os.path.join(root, rel)):
            return ("src", rel), rel
    rel = os.path.normpath(os.path.join(mod.dir, path))
    mod.resources_missing.append(rel)
    return ("src", rel), rel


def _convert_resource(mod, root, tokens):
    """RESOURCE([FORCE_TEXT] [Src Key]* [- Key=Value]*) ->
    ("file", ref, key) and ("text", key, value) items."""
    items = []
    it = iter(t for t in tokens if t not in _RESOURCE_FLAGS)
    for tok in it:
        nxt = next(it, None)
        if nxt is None:
            raise UnsupportedResource("odd number of arguments")
        if tok == "-":
            key, sep, value = _expand_resource_vars(nxt, mod).partition("=")
            if not sep:
                raise UnsupportedResource("'- %s' has no '='" % nxt)
            items.append(("text", key, value))
        else:
            ref, _ = _resource_input(mod, root, tok)
            items.append(("file", ref, _expand_resource_vars(nxt, mod)))
    return items


class _ResourceFilesGroup:
    """One run of RESOURCE_FILES paths sharing PREFIX/DEST/STRIP; `entries` are
    (ref, rootrel, key) as build/plugins/res.py:onresource_files computes them,
    duplicated keys already skipped."""

    def __init__(self, prefix, moddir):
        self.prefix = prefix
        self.moddir = moddir
        self.entries = []

    def add(self, text, ref, rootrel, prefix, dest, strip, seen):
        if dest is not None:
            name = dest
        elif strip and text.startswith(strip):
            name = prefix + text[len(strip):]
        else:
            name = prefix + text
        key = "resfs/file/" + name
        if key not in seen:
            seen.add(key)
            self.entries.append((ref, rootrel, key))


def _convert_resource_files(mod, root, tokens):
    """RESOURCE_FILES([DONT_COMPRESS] [PREFIX {prefix}] [STRIP prefix_to_strip] {path})"""
    groups, seen = [], set()
    prefix, dest, strip = "", None, None
    group = None
    it = iter(tokens)
    for tok in it:
        if tok == "DONT_COMPRESS":
            continue
        if tok in ("PREFIX", "DEST", "STRIP"):
            value = next(it, None)
            if value is None:
                raise UnsupportedResource("%s without a value" % tok)
            value = _expand_resource_vars(value, mod)
            if tok == "PREFIX":
                prefix, dest = value, None
            elif tok == "DEST":
                dest, prefix = value, ""
            else:
                strip = value
            group = None
            continue
        if group is None:
            group = _ResourceFilesGroup(prefix, mod.dir)
            groups.append(("files", group))
        ref, rootrel = _resource_input(mod, root, tok)
        group.add(tok.replace("${MODDIR}", mod.dir), ref, rootrel, prefix, dest, strip, seen)
    return groups


def _convert_all_resource_files(mod, root, tokens, with_ext):
    """ALL_RESOURCE_FILES(Ext [PREFIX {prefix}] [STRIP {strip}] Dirs...) and
    ALL_RESOURCE_FILES_FROM_DIRS([PREFIX {prefix}] [STRIP {strip}] Dirs...):
    a non-recursive glob over module-relative Dirs, expanded here once, then
    RESOURCE_FILES with STRIP ${ARCADIA_ROOT}/${MODDIR}/${STRIP}."""
    ext, prefix, strip, dirs = None, "", "", []
    it = iter(tokens)
    for tok in it:
        if tok in ("PREFIX", "STRIP"):
            value = next(it, None)
            if value is None:
                raise UnsupportedResource("%s without a value" % tok)
            value = _expand_resource_vars(value, mod)
            if tok == "PREFIX":
                prefix = value
            else:
                strip = value
        elif with_ext and ext is None:
            ext = tok
        else:
            dirs.append(_expand_resource_vars(tok, mod))
    if with_ext and ext is None:
        raise UnsupportedResource("no extension")

    root_marker = "${ARCADIA_ROOT}/"
    strip_path = root_marker + mod.dir + "/" + strip
    group, seen = _ResourceFilesGroup(prefix, mod.dir), set()
    for d in dirs:
        rel_dir = os.path.normpath(os.path.join(mod.dir, d))
        try:
            names = sorted(os.listdir(os.path.join(root, rel_dir)))
        except OSError:
            raise UnsupportedResource("no directory %s" % rel_dir)
        for n in names:
            if n.startswith(".") or (with_ext and not n.endswith("." + ext)):
                continue
            rel = os.path.join(rel_dir, n)
            if os.path.isfile(os.path.join(root, rel)):
                group.add(root_marker + rel, ("src", rel), rel, prefix, None, strip_path, seen)
    return [("files", group)]


def convert_resources(mod, root):
    """Fill mod.resources with ("file", ref, key), ("text", key, value) and
    ("files", _ResourceFilesGroup) items; an argument list we can't express is
    recorded as a non-trivial pseudo-macro, so the module is skipped with a
    reason."""
    for name, raw in mod.resource_macros:
        tokens = _resource_tokens(raw)
        try:
            if name == "RESOURCE":
                mod.resources += _convert_resource(mod, root, tokens)
            elif name == "RESOURCE_FILES":
                mod.resources += _convert_resource_files(mod, root, tokens)
            else:
                mod.resources += _convert_all_resource_files(
                    mod, root, tokens, with_ext=(name == "ALL_RESOURCE_FILES"))
        except UnsupportedResource as e:
            mod.macros.append("%s<%s>" % (name, e))
    if mod.resources and mod.no_util:
        mod.macros.append("RESOURCE+NO_UTIL")


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
        # folded further up (contrib/libs/brotli/c/dec -> //contrib/libs/brotli:dec):
        # only across dirs with no BUILD.gn, whose targets can live nowhere else
        d = os.path.dirname(owner)
        while (d not in ("", "contrib")
               and not os.path.exists(os.path.join(self.root, d, "BUILD.gn"))):
            d = os.path.dirname(d)
            # not the ancestor's own main target (zstd/programs/zstd is not //contrib/libs/zstd)
            if name != os.path.basename(d) and self.declares(d, name):
                return "//%s:%s" % (d, name)
        if self.is_external(owner) or self.is_external(os.path.dirname(owner)):
            return module_label(owner)
        return None


# --- include resolution ----------------------------------------------------

class Resolver:
    def __init__(self, root, mods, external=None):
        self.root = root
        self.mods = mods
        self.external = external
        # a file some module generates -> that module (see GENERATOR_MACROS)
        self.generated = {g: d for d, m in mods.items() for g in m.generated}
        self._cache = {}
        # include-side module dir -> the module it is folded into (INCLUDE_ROOT_REMAP)
        self.aliases = {}
        self.alias_parts = {}   # include-side module dir -> part of its alias target
        self.absorbed = {}      # module dir -> module that compiles all of it (link_foreign_sources)
        self.self_split = {}    # module dir -> the modules it PEERDIRs that include it (plan_self_split)
        self.cycles = None      # CycleGraph
        buildable = lambda t: t is not None and t.kind in KIND_MACROS and not is_test(t)
        for d, m in mods.items():
            if not buildable(m):
                continue
            for old, new in INCLUDE_ROOT_REMAP:
                if not d.startswith(old):
                    continue
                target = new + d[len(old):]
                if buildable(mods.get(target)):
                    self.aliases[d] = target
                    continue
                up = os.path.dirname(target)
                while up and not up.startswith(".") and not buildable(mods.get(up)):
                    up = os.path.dirname(up)
                part = os.path.basename(d)
                if (up and buildable(mods.get(up)) and _GN_PART_RE.match(part)
                        and part != os.path.basename(up)):
                    self.aliases[d] = up
                    self.alias_parts[d] = part
        # header -> the module that claims it: a SRCDIR module listing, in its
        # SRCS, a header that lives outside its own dir (library/cpp/charset/lite
        # lists ../codepage.h). Owned by it rather than by the dir's module,
        # unless that module lists it too or several modules claim it.
        claims = defaultdict(set)
        listed = set()
        for d, m in mods.items():
            if m.kind not in KIND_MACROS or is_test(m):
                continue
            for s in m.srcs:
                if not s.endswith(HEADER_EXTS):
                    continue
                rel = module_path(m, root, s)
                if rel.startswith(d + "/"):
                    listed.add(rel)
                else:
                    claims[rel].add(d)
        self.claims = {h: next(iter(ds)) for h, ds in claims.items()
                       if len(ds) == 1 and h not in listed}

    def alias(self, d):
        return self.aliases.get(d, d)

    def is_module(self, d):
        """True if d's ya.make module has a GN target: a buildable kind
        (LIBRARY/PROGRAM/PROTO_LIBRARY), or any kind (YQL_UDF_YDB, ...) whose
        hand-written keep-marked BUILD.gn declares the target."""
        mod = self.mods.get(d)
        if mod is None:
            return False
        if mod.kind in KIND_MACROS:
            return not is_test(mod)
        return (self.external is not None and not is_test(mod)
                and self.external.declares(d, os.path.basename(d)))

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
                if mod is None or mod.kind in KIND_MACROS or self.is_module(d):
                    return d
            if not d or d == ".":
                return None
            d = os.path.dirname(d)

    def owner(self, d):
        """The module whose GN target provides the files of module d."""
        d = self.alias(d)
        return self.absorbed.get(d, d)

    def nearest_module(self, rel_path):
        if rel_path in self.claims:
            return self.owner(self.claims[rel_path])
        owner = self._walk_up(os.path.dirname(rel_path))
        if owner is not None:
            return self.owner(owner)
        for old, new in INCLUDE_ROOT_REMAP:
            if rel_path.startswith(old):
                remapped = new + rel_path[len(old):]
                owner = self._walk_up(os.path.dirname(remapped))
                if owner is not None:
                    return self.owner(owner)
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
        # a file generated by a module (RUN_PROGRAM ... OUT ...): not in the
        # source tree, but that module's header all the same
        for rel in cands:
            if rel in self.generated:
                return rel
        # Last resort: the GLOBAL ADDINCL roots contrib publishes, so that
        # `#include <openssl/sha.h>` finds contrib/libs/openssl/include/... and
        # the dependency on //contrib/libs/openssl is derived like any other.
        if not inc.startswith((".", "/")):
            for base in CONTRIB_INCLUDE_ROOTS:
                rel = os.path.normpath(os.path.join(base, inc))
                if os.path.exists(os.path.join(self.root, rel)):
                    return rel
        return None


def module_path(mod, root, path):
    """Repo-relative path of a file a ya.make macro names: "//x" and
    "${ARCADIA_ROOT}/x" are source-root-relative; otherwise the first existing of
    SRCDIR-, module-, and source-root-relative (ymake's lookup order)."""
    if path.startswith("//"):
        return os.path.normpath(path[2:])
    if path.startswith("${ARCADIA_ROOT}/"):
        return os.path.normpath(path[len("${ARCADIA_ROOT}/"):])
    cands = [os.path.normpath(os.path.join(base, path)) for base in (_src_prefix(mod), mod.dir, "")]
    for c in cands:
        if not c.startswith("..") and os.path.exists(os.path.join(root, c)):
            return c
    return cands[0]


def merge_aliases(mods, resolver):
    """Fold every include-side alias module (Resolver.aliases) into its src
    module: its enum serialization and PEERDIRs become the src module's (its
    headers already are, via nearest_module/IncludeGraph). An alias to a part
    (Resolver.alias_parts) makes that part of the src module instead: its
    headers, sources and PEERDIRs."""
    for inc, src in sorted(resolver.aliases.items()):
        im, sm = mods[inc], mods[src]
        part = resolver.alias_parts.get(inc)
        if part is not None:
            prefix = _src_prefix(sm)
            for x in im.srcs:
                rel = module_path(im, resolver.root, x)
                if rel.endswith(HEADER_EXTS):
                    sm.part_headers[part].append(os.path.relpath(rel, prefix))
                elif rel.endswith(COMPILED_EXTS):
                    sm.foreign_srcs[rel] = part
            for p in im.peerdirs:
                p = os.path.normpath(p.rstrip("/"))
                if resolver.alias(p) != src and p not in sm.part_peerdirs[part]:
                    sm.part_peerdirs[part].append(p)
            sm.enum_headers += ["//" + module_path(im, resolver.root, h) for h in im.enum_headers]
            sm._part_of = None
            continue
        for h in im.enum_headers:
            h = "//" + module_path(im, resolver.root, h)
            if h not in sm.enum_headers:
                sm.enum_headers.append(h)
        known = {os.path.normpath(p.rstrip("/")) for p in sm.peerdirs}
        for p in im.peerdirs:
            p = os.path.normpath(p.rstrip("/"))
            if resolver.alias(p) != src and p not in known:
                sm.peerdirs.append(p)
                known.add(p)


def link_foreign_sources(mods, resolver):
    """Hand every `# gn: into <dir>[:<part>]` source to the module it names
    (Module.foreign_srcs); a module whose sources all go to one module, with
    nothing else to build, is absorbed by it (Module.absorbs,
    Module.absorbed_into): it has no target of its own. Its enum serialization
    and resources move along (a hand-written absorbing BUILD.gn has to list
    them itself)."""
    for d, mod in sorted(mods.items()):
        if not mod.src_into:
            continue
        prefix = _src_prefix(mod)
        targets = set()
        intos = set()
        for src, (into, part) in sorted(mod.src_into.items()):
            into = resolver.alias(into)
            other = mods.get(into)
            if other is None or into == d or not is_real(other):
                mod.macros.append("GN<into unknown module %s>" % into)
                continue
            if part is not None and not _GN_PART_RE.match(part):
                mod.macros.append("GN<into bad part name %r>" % part)
                continue
            other.foreign_srcs[os.path.normpath(os.path.join(prefix, src))] = part
            other._part_of = None
            targets.add(into)
            intos.add((into, part))
        # nothing of its own left to build: absorbed, the module has no target
        # of its own, the one that has its code stands for it. Its enum
        # serialization and resources go along to a main target (a part carries
        # none).
        if (len(intos) != 1
                or [s for s in mod.compiled_srcs() if s.endswith(COMPILED_EXTS + PROTO_EXTS)]):
            continue
        into, part = intos.pop()
        if mod.resources:
            if part is not None:
                continue
            mods[into].resources += mod.resources
            mod.resources = []
        if mod.enum_headers and part is None:
            mods[into].enum_headers += ["//" + module_path(mod, resolver.root, h)
                                        for h in mod.enum_headers]
            mod.enum_headers = []
        if not mod.enum_headers:
            mods[into].absorbs.add(d)
            mod.absorbed_into = module_part_label(into, part)
            if part is None:
                # its headers are the absorbing module's code now
                resolver.absorbed[d] = into


class IncludeGraph:
    """The #include graph of every real module in the tree, built once before
    Pass 1 so that no answer depends on which modules the current run happens
    to generate.

      * files[d] -- files whose includes define the deps of module d: exactly
        its SRCS (incl. .proto), plus every header OWNED by d (nearest_module)
        that is reachable via #include from the files of ANY real module --
        d's own sources, or a consumer's. A header that only other modules
        include (e.g. a header-only helper no .cpp of d touches) is still d's
        public face, so its includes must count. Test helpers (d/ut/...,
        which nearest_module attributes to d because tests are not real
        modules, or *_ut*.h) join d only from d's own files: a test-support
        library that includes them must not drag test deps into d.
      * consumed -- headers included from a file of a module other than their
        owner; their owner may have to re-export what they pull
        (finalize_publicity).
    """

    def __init__(self, resolver):
        self.resolver = resolver
        self.files = defaultdict(list)
        self.consumed = set()
        self._file_set = defaultdict(set)
        self._includes = {}

    def includes(self, f):
        """[(include as written, resolved repo-relative path or None)] of the
        lines surviving preprocessing; generated *.pb.h map to their .proto."""
        if f in self._includes:
            return self._includes[f]
        out = []
        try:
            with open(os.path.join(self.resolver.root, f), encoding="utf-8",
                      errors="ignore") as fh:
                lines = fh.readlines()
        except OSError:
            lines = []
        fdir = os.path.dirname(f)
        for line in active_lines(lines):
            m = INCLUDE_RE.match(line)
            if not m:
                continue
            inc = m.group(2)
            rel = self.resolver.resolve(inc, m.group(1) == '"', fdir)
            if rel is None and PB_RE.search(inc):
                rel = self.resolver.resolve(PB_RE.sub(".proto", inc), False, fdir)
            out.append((inc, rel))
        self._includes[f] = out
        return out

    _TEST_DIR_RE = re.compile(r"^(ut|ut_.*|.*_ut|test|tests|testing|benchmark)$")
    _TEST_FILE_RE = re.compile(r"(^|_)ut(_|\.|$)")

    def _is_test_header(self, rel, owner):
        """True if `rel` is test support rather than part of `owner`: a test
        module's ya.make sits between them, or (the Arcadia convention for
        test helpers without a ya.make of their own) a ut/ test/ ... dir lies
        between them or the file name says _ut (flat_ut_client.h)."""
        if self._TEST_FILE_RE.search(os.path.basename(rel)):
            return True
        d = os.path.dirname(rel)
        while d and d != owner:
            mod = self.resolver.mods.get(d)
            if (mod is not None and is_test(mod)) or self._TEST_DIR_RE.match(os.path.basename(d)):
                return True
            d = os.path.dirname(d)
        return False

    def owners(self, d):
        """Modules (other than d itself, util and contrib) whose files the
        files of d include or, for .proto sources, import."""
        out = set()
        for f in self.files[d]:
            if f.endswith(PROTO_EXTS):
                try:
                    with open(os.path.join(self.resolver.root, f), encoding="utf-8",
                              errors="ignore") as fh:
                        lines = fh.readlines()
                except OSError:
                    continue
                rels = [self.resolver.resolve(m.group(1), False, os.path.dirname(f))
                        for m in map(IMPORT_RE.match, lines) if m]
            else:
                rels = [rel for _, rel in self.includes(f)]
            for rel in rels:
                owner = rel and self.resolver.nearest_module(rel)
                if owner:
                    out.add(owner)
        out.discard(d)
        return {o for o in out
                if o != "util" and not o.startswith(("util/", "contrib/"))}

    def _add(self, d, f, queue):
        if f not in self._file_set[d]:
            self._file_set[d].add(f)
            self.files[d].append(f)
            queue.append((d, f))

    def build(self, mods, dirs):
        queue = []
        for d in sorted(dirs):
            mod = mods[d]
            srcdir = _src_prefix(mod)
            owner = self.resolver.owner(d)    # an include-side alias / absorbed module seeds its owner
            if owner not in dirs:
                continue
            for src in mod.compiled_srcs():
                if src.endswith(COMPILED_EXTS + HEADER_EXTS + PROTO_EXTS):
                    self._add(owner, os.path.join(srcdir, src), queue)
            for f in sorted(mod.foreign_srcs):          # GN_DIRECTIVES `into`
                self._add(owner, f, queue)
            for headers in mod.part_headers.values():   # GN_DIRECTIVES
                for h in headers:
                    self._add(owner, os.path.normpath(os.path.join(srcdir, h)), queue)
        while queue:
            d, f = queue.pop()
            if not f.endswith(COMPILED_EXTS + HEADER_EXTS):
                continue
            for _, rel in self.includes(f):
                if rel is None or not rel.endswith(HEADER_EXTS):
                    continue
                owner = self.resolver.nearest_module(rel)
                if owner is None:
                    continue
                if owner != d:
                    self.consumed.add(rel)
                if owner in dirs and (owner == d or not self._is_test_header(rel, owner)):
                    self._add(owner, rel, queue)
        return self


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
        self.header_internal = defaultdict(set) # own header path -> own headers it includes
        self.proto_public = set()               # proto imports (always public)
        self.sources = []                       # repo-relative, non-proto
        self.proto_sources = []                 # repo-relative .proto sources
        self.enum_headers = []                  # repo-relative (GENERATE_ENUM_SERIALIZATION)
        self.resources = list(mod.resources)    # see convert_resources
        self.has_srcs = bool(mod.srcs or mod.foreign_srcs)  # any SRCS at all (even headers only)
        self.peerdir_only = set()               # deps from PEERDIR with no #include behind
        self.host = mod.dir                      # set during merge planning
        self.parts = []                         # TargetSpecs of GN_DIRECTIVES parts
        self.part = None                        # this spec's part name, if it is one
        self.local_consumed = set()             # own headers included by another part
        self.explicit = set()                   # deps written in ya.make GN_DIRECTIVES: never commented
        self.always_public = set()              # public whatever the headers say (a facade's, split_self)
        self.slot_provides = sorted(set(mod.provides))  # PROVIDES() of the module
        self.slot_interface = None              # the link slot the module is the interface of, `# gn: slot ...`
        self.slot_headers = []                  # its headers, repo-relative
        self.slot_flags_from = None             # a source lending its flags to parsing them
        self.slot_module_interfaces = []        # slots whose interface is another target of the module
        self.proto_plugins = list(mod.proto_plugins)   # CPP_PROTO_PLUGIN0
        self.yql_abi = mod.yql_abi              # YQL_LAST_ABI_VERSION / YQL_ABI_VERSION


def module_part_label(d, part):
    return module_label(d) if part is None else "%s:%s" % (module_label(d), part)


def owner_label(owner, rel, resolver, external):
    """Label of the target providing repo-relative file `rel` of module
    `owner`: a GN_DIRECTIVES part, the module itself, or (contrib) its external
    label."""
    if owner.startswith("contrib/"):
        return external.label(owner) or module_label(owner)
    mod = resolver.mods.get(owner)
    return module_part_label(owner, mod.part_of(rel) if mod is not None else None)


def _record(spec, rel, owner, target_dir, header_file, report, external, resolver,
            from_proto=False):
    """Register one resolved include/import edge of `target_dir` (of `spec`: the
    module's main target or one of its GN_DIRECTIVES parts)."""
    if owner is None:
        # Resolved to a real file, but no ancestor declares a buildable
        # module (and no INCLUDE_ROOT_REMAP applies) -- can't derive a dep.
        report.unresolved[target_dir].add(rel)
        return
    if owner == target_dir:
        mod = resolver.mods.get(owner)
        rel_part = mod.part_of(rel) if mod is not None else None
        if rel_part == spec.part:
            if header_file is not None and rel.endswith(HEADER_EXTS):
                spec.header_internal[header_file].add(rel)
            return
        # another part of the same module: an ordinary dep on that target,
        # whose header is thereby "consumed" (see finalize_publicity); the
        # main target's headers of a split module are its SELF_PART's
        if rel_part is None and owner in resolver.self_split:
            rel_part = SELF_PART
        label = module_part_label(owner, rel_part)
        spec.deps.add(label)
        spec.local_consumed.add(rel)
        if header_file is not None:
            spec.header_owners[header_file].add(label)
        return
    target_mod = resolver.mods.get(target_dir)
    if target_mod is not None and owner in target_mod.absorbs:
        # all of owner's code is compiled here (`# gn: into`): its headers
        # are this module's own, a dep on it would only close a cycle
        return
    if owner == "util" or owner.startswith("util/"):
        return
    # A target we generate keeps the plain //owner here; if Pass 2 folds it into
    # a keep parent, the remap rewrites it then. Contrib is never generated, so
    # its (possibly folded) external label must be resolved now.
    label = owner_label(owner, rel, resolver, external)
    if label == module_label(owner) and target_dir in resolver.self_split.get(owner, ()):
        label = module_part_label(owner, SELF_PART)   # see plan_self_split
    if label in PROTO_PROVIDED_LABELS and (from_proto or spec.kind == "PROTO_LIBRARY"):
        return  # protobuf_library template injects protobuf/grpc itself
    if owner.startswith("contrib/"):
        report.contrib_deps[target_dir].add(label)
    spec.deps.add(label)
    if header_file is not None:
        spec.header_owners[header_file].add(label)


def peerdir_owners(mod, resolver, peerdirs=None):
    """Module dirs mod PEERDIRs (or `peerdirs`) that GN can depend on: a
    buildable non-test module (LIBRARY/PROGRAM/PROTO_LIBRARY), or any contrib
    module; util and mod itself excluded."""
    out = []
    for p in (mod.peerdirs if peerdirs is None else peerdirs):
        p = resolver.alias(os.path.normpath(p.rstrip("/")))
        if p == mod.dir or p == "util" or p.startswith("util/") or p in out:
            continue
        if p.startswith("contrib/"):
            if os.path.exists(os.path.join(resolver.root, p, "ya.make")):
                out.append(p)
            continue
        if resolver.is_module(p):
            out.append(p)
    return out


SELF_PART = "_self"


class CycleGraph:
    """The module graph cycles are looked for in: PEERDIRs (reach) and the
    #includes of a module's main-target headers (includers)."""

    def __init__(self, mods, resolver, graph, dirs):
        self.mods, self.resolver = mods, resolver
        self._closure = {}
        self.includers = defaultdict(set)   # module -> modules including its main target's headers
        for c in dirs:
            for f in graph.files[c]:
                if f.endswith(PROTO_EXTS):
                    continue
                for _, rel in graph.includes(f):
                    owner = rel and resolver.nearest_module(rel)
                    if not owner or owner == c or owner.startswith(("contrib/", "util")):
                        continue
                    om = mods.get(owner)
                    if om is not None and om.part_of(rel) is None:
                        self.includers[owner].add(c)

    def reach(self, p):
        """Modules p PEERDIRs, directly or not."""
        if p in self._closure:
            return self._closure[p]
        resolver = self.resolver
        out, seen, stack = set(), {p}, [p]
        while stack:
            m = self.mods.get(stack.pop())
            if m is None:
                continue
            for q in m.peerdirs:
                q = os.path.normpath(q.rstrip("/"))
                # a PEERDIR on an include-side module folded into its src one
                # (merge_aliases) is on its headers and their PEERDIRs only
                via = q if q in resolver.aliases and q not in resolver.alias_parts else None
                q = resolver.absorbed.get(resolver.alias(q), resolver.alias(q))
                if q == p or q.startswith(("contrib/", "util")) or not resolver.is_module(q):
                    continue
                out.add(q)
                nxt = via or q
                if nxt in seen:
                    continue
                seen.add(nxt)
                if via is None and q in self._closure:
                    out |= self._closure[q]
                else:
                    stack.append(nxt)
        self._closure[p] = out
        return out

    def closers(self, p):
        """Modules p PEERDIRs that include its headers: in ya fine (an #include
        needs no PEERDIR, the link is whole-program), a cycle in GN."""
        return self.includers.get(p, set()) & self.reach(p)


def _is_variant(root, d):
    """A module built from a shared ya.make.inc (llvm16 / no_llvm): its sources
    are another configuration of the same code, never merged into anything."""
    try:
        with open(os.path.join(root, d, "ya.make"), encoding="utf-8", errors="ignore") as f:
            return bool(re.search(r"INCLUDE\([^)]*ya\.make\.inc\s*\)", f.read()))
    except OSError:
        return False


def plan_merge_groups(mods, resolver, cg, external):
    """Cycles inside one directory, merged away: {P: modules to absorb}.

    Module P is in a cycle when some module it PEERDIRs includes its headers
    (CycleGraph.closers). The modules to merge into P are the least set M
    holding P such that no module outside M both includes a header of M and
    is PEERDIRed by M: grown by such modules and every module on the PEERDIR
    paths from M to them. M is merged -- all of it is compiled by P's target
    (absorb) -- when it lies in P's directory and every module of it can be
    merged (a plain generated LIBRARY; see `mergeable`). Other cycles are
    between libraries of their own: for those see ANTI_CYCLE_FACADE."""
    root = resolver.root

    def inside(d, p):
        return d == p or d.startswith(p + "/")

    def mergeable(d, p):
        """d can be (d == p: absorb others as) a plain generated LIBRARY; a
        module absorbed into p has nothing p's target can't carry"""
        m = mods.get(d)
        if (m is None or m.kind != "LIBRARY" or not is_trivial(m) or is_test(m)
                or m.anti_cycle_facade or m.absorbed_into is not None
                or external.is_external(d) or _is_variant(root, d)
                or (external.is_external(os.path.dirname(d))
                    and external.declares(os.path.dirname(d), os.path.basename(d)))):
            return False
        # (flags of its own, ignored for now, would then be the absorbing
        # module's: -mavx2 of an AVX2 variant)
        abi = lambda x: None if x == "current" else x
        return d == p or not (
            m.parts() or m.resources or m.provides or m.src_into
            or any(part is not None for part in m.foreign_srcs.values())
            or abi(m.yql_abi) != abi(mods[p].yql_abi)
            or {"CFLAGS", "CXXFLAGS", "CONLYFLAGS"} & set(m.macros)
            or [s for s in m.srcs if s.endswith(PROTO_EXTS)])

    def group(p):
        members = {p}
        while True:
            closing = {x for m in members for x in cg.includers.get(m, ())
                       if x not in members and any(x in cg.reach(y) for y in members)}
            if not closing:
                return members
            on_path = {q for m in members for q in cg.reach(m)
                       if q not in members and q not in closing and cg.reach(q) & closing}
            new = closing | on_path
            if not all(inside(d, p) and mergeable(d, p) for d in new):
                return None
            members |= new

    groups, taken = {}, set()
    candidates = [p for p, m in mods.items()
                  if m.kind == "LIBRARY" and not is_test(m) and cg.closers(p)]
    for p in sorted(candidates, key=lambda d: (d.count("/"), d)):
        if p in taken or not mergeable(p, p):
            continue
        members = group(p)
        if members is None:
            continue
        groups[p] = members - {p}
        taken |= members
    return groups


def absorb(mods, resolver, d, into):
    """Compile all of module d in module `into` (as `# gn: into` of all its
    sources would): d has no target of its own, its headers are `into`'s, its
    enum serialization and PEERDIRs go along (gen_spec)."""
    mod, other = mods[d], mods[into]
    prefix = _src_prefix(mod)
    for src in mod.compiled_srcs():
        if src.endswith(COMPILED_EXTS):
            mod.src_into[src] = (into, None)
            other.foreign_srcs[os.path.normpath(os.path.join(prefix, src))] = None
    # what d compiles of other modules (`# gn: into d`) goes along
    for f in mod.foreign_srcs:
        other.foreign_srcs[f] = None
    mod.foreign_srcs = {}
    for a in mod.absorbs:
        other.absorbs.add(a)
        mods[a].absorbed_into = module_label(into)
        resolver.absorbed[a] = into
    mod.absorbs = set()
    other._part_of = None
    other.enum_headers += ["//" + module_path(mod, resolver.root, h) for h in mod.enum_headers]
    mod.enum_headers = []
    other.absorbs.add(d)
    mod.absorbed_into = module_label(into)
    resolver.absorbed[d] = into


# `# gn: anti-cycle facade`, a standalone line of a module's ya.make: the
# module is an active link of a cycle between libraries (some module it
# PEERDIRs includes its headers -- the code smells), split in two to break
# it: its content goes to the part SELF_PART, which those modules depend on;
# its own target is a facade re-exporting that part and its PEERDIRs, which
# every other consumer depends on.
ANTI_CYCLE_FACADE = "anti-cycle facade"


def plan_self_split(mods, cg):
    """{P: the modules P PEERDIRs that include it} for every ANTI_CYCLE_FACADE
    module (an empty set: the directive has become useless)."""
    return {p: cg.closers(p) for p, m in mods.items() if m.anti_cycle_facade}


# The link slot (build/gn/link_slots.gni) an ALLOCATOR_IMPL() module provides:
# NMalloc::MallocInfo(), which ya.make's ALLOCATOR() of a PROGRAM picks.
ALLOCATOR_SLOT = "allocator"

def link_slot_interfaces(root, mods):
    """The link slots with an interface: `# gn: slot` in a ya.make, or
    link_slot_interface in a hand-written BUILD.gn."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import link_slot_index
    return ({slot for m in mods.values() for slot in m.slot_headers}
            | set(link_slot_index.index(root, "gn")[1]))


def drop_slotless_provides(root, mods, report):
    """PROVIDES() of a name no module is the interface of is ya's check that
    a program links one such module at most, not a link slot: nothing refers
    to it weakly, so it gets no link_slot_provides (link_slot_index.py fails
    `gn gen` on a slot with providers and no interface)."""
    slots = link_slot_interfaces(root, mods)
    for d, m in mods.items():
        for name in [p for p in m.provides if p not in slots]:
            report.provides_no_slot.append((d, name))
        m.provides = [p for p in m.provides if p in slots]


def link_slot_providers(root, mods):
    """{(slot, value): {module dir, ...}}: the providers of the link slots
    (build/gn/link_slots.gni), which a linked_executable() selects by
    "<slot>=<value>". Those of ya.make -- PROVIDES(), ALLOCATOR_IMPL(); the
    value is the target's name, the module's dir name -- whether generated yet
    or not, plus those declared by hand in BUILD.gn files, gathered the way
    `gn gen` does (build/gn/scripts/link_slot_index.py)."""
    out = defaultdict(set)
    for d, m in mods.items():
        if m.kind in KIND_MACROS and not is_test(m):
            for slot in m.provides:
                out[(slot, os.path.basename(d))].add(d)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import link_slot_index
    for key, labels in link_slot_index.scan(root, "gn").items():
        out[key].update(l[2:].split(":")[0] for l in labels)
    return out


_PROGRAM_NAME_RE = re.compile(r"^[A-Za-z0-9_][A-Za-z0-9_.+-]*$")


def gen_spec(mod, resolver, graph, report, external):
    target_dir = mod.dir
    if mod.transitive_headers_no:
        report.transitive_headers_no.append(target_dir)
    spec = TargetSpec(mod)
    if mod.kind == "PROGRAM" and _PROGRAM_NAME_RE.match(mod.name or ""):
        # PROGRAM(<name>): the binary is called so, and so is the target
        # (the executable's output name is its target name); the label
        # //<dir> is remapped to //<dir>:<name> in Pass 2
        spec.name = mod.name
    parts = {}
    for name in mod.parts():
        part = TargetSpec(mod)
        part.name = part.part = name
        part.kind = "LIBRARY"
        part.resources = []
        parts[name] = part
    spec.parts = [parts[n] for n in sorted(parts)]
    for slot, hs in mod.slot_headers.items():   # one per target, see _check_gn_parts
        paths = sorted(module_path(mod, resolver.root, h) for h in hs)
        # the headers of a part (`# gn: <part> headers ...`): the part is the
        # interface, the rest of the module is not
        owners = {mod.part_of(h) for h in paths}
        slot_spec = parts[owners.pop()] if len(owners) == 1 and None not in owners else spec
        slot_spec.slot_interface = slot
        slot_spec.slot_headers = paths
    # the module's code defines some functions its slot headers declare: every
    # target of it is the interface of the module's slots, not only the one
    # holding the headers
    for s in [spec] + spec.parts:
        s.slot_module_interfaces = sorted(t.slot_interface for t in [spec] + spec.parts
                                          if t is not s and t.slot_interface)
    for slot_spec in [spec] + spec.parts:
        if not slot_spec.slot_headers:
            continue
        # for a header-only interface: a source of its dir, compiled with the
        # headers' include dirs (by a variant of the module)
        try:
            own = sorted(f for f in os.listdir(os.path.join(resolver.root, target_dir))
                         if f.endswith((".cpp", ".cc")) and "_ut" not in f)
        except OSError:
            own = []
        # preferably the implementation of a slot header (yt_codec_cg.cpp
        # for yt_codec_cg.h): compiled as C++ by every variant
        stems = {os.path.splitext(os.path.basename(h))[0] for h in slot_spec.slot_headers}
        own.sort(key=lambda f: os.path.splitext(f)[0] not in stems)
        slot_spec.slot_flags_from = os.path.join(target_dir, own[0]) if own else None

    for f in graph.files[target_dir]:
        fdir = os.path.dirname(f)
        owner_spec = parts.get(mod.part_of(f), spec)
        if any(s in os.path.basename(f) for s in ARCH_SUFFIXES):
            report.arch_files.append(f)
        if f.endswith(PROTO_EXTS):
            try:
                with open(os.path.join(resolver.root, f), encoding="utf-8",
                          errors="ignore") as fh:
                    lines = fh.readlines()
            except OSError:
                continue
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
                _record(spec, rel, owner, target_dir, None, report, external, resolver,
                        from_proto=True)
                if owner and owner != target_dir:
                    label = module_label(owner)
                    if owner.startswith("contrib/"):
                        label = external.label(owner) or label
                    if label not in PROTO_PROVIDED_LABELS:
                        spec.proto_public.add(label)
            continue
        header_file = f if f.endswith(HEADER_EXTS) else None
        if header_file is not None:
            owner_spec.header_owners.setdefault(header_file, set())
        for inc, rel in graph.includes(f):
            if rel is None:
                if "/" in inc:
                    report.unresolved[target_dir].add(inc)
                continue
            _record(owner_spec, rel, resolver.nearest_module(rel), target_dir,
                    header_file, report, external, resolver)

    included = set(spec.deps)   # what the module's own files #include
    if mod.use_common_google_apis:
        spec.proto_public.add(GOOGLEAPIS_COMMON_PROTOS_LABEL)

    peer = {module_part_label(resolver.alias(os.path.normpath(p.rstrip("/"))),
                              resolver.alias_parts.get(os.path.normpath(p.rstrip("/"))))
            for p in mod.peerdirs}
    peer.discard("//util")
    report.peer_diff[target_dir] = (sorted(peer - spec.deps), sorted(spec.deps - peer))

    # PEERDIRs no #include leads to are still real deps (link-only: static
    # registration, an implementation of someone else's header, ...): add them,
    # remembering which ones came from PEERDIR alone (rendered "# peerdir only").
    # One on a module of SRCS(GLOBAL ...) or RESOURCE() is never optional: ya
    # links such a module whenever it is PEERDIRed, nothing references its
    # code (static registration, NResource)
    # (when there is a target to link: a module not generated has none)
    def linked_always(d):
        m = resolver.mods.get(d)
        return (m is not None and (m.global_srcs or bool(m.resource_macros))
                and (is_trivial(m) or external.is_external(d)))

    # A PEERDIR on an include-side module aliased to a part means that part.
    via_alias, direct = defaultdict(set), set()
    for p in mod.peerdirs:
        p = os.path.normpath(p.rstrip("/"))
        if p in resolver.alias_parts:
            via_alias[resolver.alias(p)].add(resolver.alias_parts[p])
        else:
            direct.add(resolver.alias(p))
    for owner in peerdir_owners(mod, resolver):
        if owner in mod.absorbs:
            continue    # its code is compiled here
        if mod.peerdir_parts.get(owner):
            labels = [module_part_label(owner, p) for p in mod.peerdir_parts[owner]]
        elif owner.startswith("contrib/"):
            labels = [external.label(owner) or module_label(owner)]
        elif target_dir in resolver.self_split.get(owner, ()):
            # (only through an include-side module: its headers, see plan_self_split)
            labels = [module_part_label(owner, SELF_PART)] if owner in direct else []
        else:
            labels = [module_label(owner)] if owner in direct else []
        labels += [module_part_label(owner, p) for p in sorted(via_alias.get(owner, ()))]
        for label in labels:
            if mod.peerdir_parts.get(owner):
                spec.explicit.add(label)
            if ((label in PROTO_PROVIDED_LABELS and mod.kind == "PROTO_LIBRARY")
                    or label in spec.deps or label in spec.proto_public):
                continue
            spec.deps.add(label)
            spec.peerdir_only.add(label)
            if linked_always(owner):
                spec.explicit.add(label)

    # PEERDIRs of an include-side module made a part of this one (merge_aliases)
    for name, dirs in sorted(mod.part_peerdirs.items()):
        t = parts[name]
        for owner in peerdir_owners(mod, resolver, dirs):
            label = (external.label(owner) or module_label(owner)
                     if owner.startswith("contrib/") else module_label(owner))
            if label not in t.deps:
                t.deps.add(label)
                t.peerdir_only.add(label)
                if linked_always(owner):
                    t.explicit.add(label)

    # PEERDIRs of a module absorbed here (`# gn: into` of all its sources):
    # its code is compiled here, and so are its link deps
    for a in sorted(mod.absorbs):
        for owner in peerdir_owners(resolver.mods[a], resolver):
            if owner == target_dir or owner in mod.absorbs:
                continue
            label = (external.label(owner) or module_label(owner)
                     if owner.startswith("contrib/") else module_label(owner))
            if label not in spec.deps and label not in spec.proto_public:
                spec.deps.add(label)
                spec.peerdir_only.add(label)
                if linked_always(owner):
                    spec.explicit.add(label)

    # `# gn: [<part>] peerdir <dir>[:<part>]`: link-only deps ya.make can't say
    for own, d, sub in mod.gn_peerdirs:
        t = parts.get(own, spec)
        label = module_part_label(d, sub)
        t.explicit.add(label)
        if label not in t.deps:
            t.deps.add(label)
            t.peerdir_only.add(label)

    # the main target keeps depending on its parts: in ya they are the module,
    # whose consumers get all of its code (a part implements what the module's
    # headers declare)
    # -- except a header-only interface of a link slot: its providers are
    # built on top of the module, the includers of its headers depend on it
    code_parts = set(mod.src_parts.values()) | {p for p in mod.foreign_srcs.values() if p}
    for part in spec.parts:
        if part.slot_interface and part.part not in code_parts:
            continue
        spec.deps.add(module_part_label(target_dir, part.part))
        spec.explicit.add(module_part_label(target_dir, part.part))
        spec.always_public.add(module_part_label(target_dir, part.part))

    srcdir = _src_prefix(mod)
    own = [s for s in mod.compiled_srcs() if s.endswith(COMPILED_EXTS)]
    spec.sources = [os.path.join(srcdir, s) for s in own if s not in mod.src_parts]
    spec.sources += sorted(f for f, p in mod.foreign_srcs.items() if p is None)
    for part in spec.parts:
        part.sources = [os.path.join(srcdir, s) for s in own if mod.src_parts.get(s) == part.part]
        part.sources += sorted(f for f, p in mod.foreign_srcs.items() if p == part.part)
    spec.proto_sources = [os.path.join(srcdir, s)
                           for s in mod.srcs if s.endswith(PROTO_EXTS)]
    # an enum's serialization goes with its header: to the part it belongs to
    enums = [module_path(mod, resolver.root, h) for h in mod.enum_headers]
    by_part = {part.part: part for part in spec.parts}
    for h in enums:
        by_part.get(mod.part_of(h), spec).enum_headers.append(h)
    if mod.resources_missing:
        report.resources_missing[target_dir].update(mod.resources_missing)
    if target_dir in resolver.self_split:
        split_self(spec, included, resolver)
    return spec


def split_self(spec, included, resolver):
    """Split an ANTI_CYCLE_FACADE module: its content -- sources, headers and
    what they #include -- moves to the part SELF_PART; the module's own target
    keeps the rest (PEERDIRs, edges to its parts) and re-exports all of it,
    the part included. The part keeps the module's PEERDIRs too (link deps of
    its code), all but those leading back to the modules that include it."""
    self_spec = TargetSpec.__new__(TargetSpec)
    self_spec.__dict__.update(spec.__dict__)
    self_spec.name = self_spec.part = SELF_PART
    self_spec.kind = "LIBRARY"
    self_spec.parts = []
    self_spec.deps = set(included)
    self_spec.peerdir_only = set()
    self_spec.explicit = spec.explicit & included
    self_spec.always_public = set()
    self_spec.header_owners = spec.header_owners
    self_spec.header_internal = spec.header_internal
    self_spec.local_consumed = spec.local_consumed

    # a PEERDIR on a closer is the cycle; one that only reaches a closer is
    # as good as a derived dep reaching it (those all go to :_self): gn
    # decides, and a new edge starts commented anyway
    closers = resolver.self_split[spec.dir]
    for l in spec.peerdir_only:
        d = l[2:].partition(":")[0]
        if l.startswith("//") and d != spec.dir and (d.startswith("contrib/") or d not in closers):
            self_spec.deps.add(l)
            self_spec.peerdir_only.add(l)
            if l in spec.explicit:
                self_spec.explicit.add(l)

    label = module_part_label(spec.dir, SELF_PART)
    own_parts = {module_part_label(spec.dir, p.part) for p in spec.parts}
    spec.deps = (spec.deps - included) | own_parts | {label}
    spec.explicit = (spec.explicit - included) | {label}
    spec.always_public = spec.always_public | spec.deps
    spec.header_owners = defaultdict(set)
    spec.header_internal = defaultdict(set)
    spec.local_consumed = set()
    spec.proto_public = set()
    spec.sources, spec.proto_sources, spec.enum_headers, spec.resources = [], [], [], []
    spec.slot_provides, spec.proto_plugins, spec.yql_abi = [], [], None
    spec.parts = spec.parts + [self_spec]


def finalize_publicity(specs, consumed):
    """A dependency is public_deps iff it is reached through the target's OWN
    headers from a header that some other module includes (`consumed`, over
    the whole tree -- see IncludeGraph). The walk follows the target's own
    headers transitively: a consumed foo.h that includes its private
    foo-inl.h / defs.h exposes what those include too. Proto imports are
    always public.

    If the module itself has .proto sources and isn't a PROTO_LIBRARY, those
    sources are split out into a sibling protobuf_library("private_proto")
    (see render_spec) -- proto-import deps belong to that sub-target, not
    here."""
    for spec in specs:
        split_proto = spec.kind != "PROTO_LIBRARY" and spec.proto_sources
        pub = set() if split_proto else set(spec.proto_public)
        exposed = [h for h in spec.header_owners if h in consumed]
        seen = set(exposed)
        while exposed:
            header = exposed.pop()
            pub |= spec.header_owners.get(header, set())
            for inner in spec.header_internal.get(header, ()):
                if inner not in seen:
                    seen.add(inner)
                    exposed.append(inner)
        pub |= spec.always_public
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

    remap, absorbed, dropped, unmerged = {}, set(), set(), []
    for P, spec in by_dir.items():
        kids = children.get(P, [])
        if not kids:
            continue
        nonleaf = [os.path.basename(c) + ":non-leaf" for c in kids if not leaf(c)]
        if nonleaf:
            report.merge_blocked.append((P, nonleaf))
            continue
        # a module split into a facade + SELF_PART keeps its own file: folded,
        # its part would be //P:<child>__self
        split = [c for c in kids if any(t.part == SELF_PART for t in by_dir[c].parts)]
        unmerged += [(c, P) for c in split]
        kids = [c for c in kids if c not in split]
        if not kids:
            continue
        taken = {spec.name} | {part.name for part in spec.parts}
        for c in kids:
            absorbed.add(c)
            cs = by_dir[c]
            for t in [cs] + cs.parts:
                t.host = P
                if t.name in taken:
                    t.name = os.path.basename(P) + "_" + t.name
                taken.add(t.name)
                remap[module_part_label(c, t.part)] = "//%s:%s" % (P, t.name)
        if (not spec.sources and not spec.proto_sources
                and not spec.deps and not spec.public_deps
                and not spec.enum_headers and not spec.resources):
            dropped.add(P)
        report.merged.append((P, sorted(by_dir[c].name for c in kids)))
    report.unmerged = sorted(unmerged)
    return remap, absorbed, dropped, unmerged


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
            if leaf(c) and not by_dir[c].parts and external.declares(P, name):
                label = "//%s:%s" % (P, name)
                remap[module_label(c)] = label
                collapsed.add(c)
                report.keep_collapsed.append((c, label))
    return remap, collapsed


# --- rendering -------------------------------------------------------------

_TARGET_RE = re.compile(
    r'(?:library|source_set|executable|linked_executable|protobuf_library|group)'
    r'\(\s*"([^"]+)"\s*\)\s*\{')


def _extract_list(body, key):
    m = re.search(r'(?<![\w])' + key + r'\s*=\s*\[(.*?)\]', body, re.S)
    return set(re.findall(r'"([^"]+)"', m.group(1))) if m else set()


def parse_existing_buildgn(path):
    """The target names an existing BUILD.gn declares, with the labels of their
    deps/public_deps (commented or not). Return {target_name: (deps, public_deps)}."""
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


def _list_uncommented(body, key):
    """Labels in body's `key = [ ... ]` list that are NOT commented out. A label
    counts as commented when a `#` precedes it on its line (GN comments run to
    end of line), so we keep only quoted strings sitting before any `#`."""
    m = re.search(r'(?<![\w])' + key + r'\s*=\s*\[(.*?)\]', body, re.S)
    if not m:
        return set()
    out = set()
    for line in m.group(1).splitlines():
        code = line.split("#", 1)[0]
        out.update(re.findall(r'"([^"]+)"', code))
    return out


def parse_existing_buildgn_uncommented(path):
    """Per target, the set of deps/public_deps labels left UNcommented in the
    existing file -- i.e. the dependencies the user has manually re-enabled.
    Everything else (commented, or absent) is re-emitted commented out on regen.
    Comment state is the only thing taken from the existing file: whether a
    label lands in deps or public_deps is always recomputed
    (finalize_publicity), regardless of comments."""
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
        out[m.group(1)] = (_list_uncommented(body, "deps")
                           | _list_uncommented(body, "public_deps"))
    return out


# A hand-written config a target pulls in: the generator cannot derive compiler
# flags from ya.make, so `config("<target>_private_config")` (the target's own
# `configs`) and `config("<target>_public_config")` (its `public_configs`, e.g.
# an ADDINCL(GLOBAL ...)) in an existing BUILD.gn are carried over verbatim
# (see render_spec).
PRIVATE_CONFIG_SUFFIX = "_private_config"
PUBLIC_CONFIG_SUFFIX = "_public_config"
_CONFIG_RE = re.compile(r'^[ \t]*config\(\s*"([^"]+)"\s*\)\s*\{', re.M)


def parse_existing_configs(path):
    """{name: the config("name") {...} block, verbatim} of an existing file."""
    try:
        with open(path, encoding="utf-8") as f:
            text = f.read()
    except OSError:
        return {}
    out = {}
    for m in _CONFIG_RE.finditer(text):
        depth, j, n = 1, m.end(), len(text)
        while j < n and depth:
            depth += {"{": 1, "}": -1}.get(text[j], 0)
            j += 1
        out[m.group(1)] = text[m.start():j].strip()
    return out


def dep_sort_key(label):
    """Order inside deps/public_deps: local ":target" labels first, then all the
    rest; alphabetical within each group. `gn format` sorts a list this way too
    -- but it treats a commented-out entry as a comment attached to the *next*
    label and moves the two together, which scatters our disabled deps. The
    order is therefore re-established by normalize_dep_lists after formatting,
    with commented and uncommented entries ranked exactly alike."""
    return (0 if label.startswith(":") else 1, label)


# One entry of a rendered deps/public_deps list: `"//foo/bar",`, optionally
# commented out (a dep the generator derived but left disabled).
_DEP_ENTRY_RE = re.compile(r'^([ \t]*)(#\s*)?"([^"]+)",?[ \t]*(#.*)?$')
# A multi-line `deps = [` / `public_deps = [` block, closed by `]` at the same
# indentation as the assignment.
_DEP_BLOCK_RE = re.compile(
    r'^(?P<indent>[ \t]*)(?P<key>public_deps|deps)\s*=\s*\[[ \t]*\n'
    r'(?P<body>.*?)'
    r'^(?P=indent)\]',
    re.M | re.S)


def normalize_dep_lists(text):
    """Re-emit every multi-line deps/public_deps list in `text` with no blank
    lines and in dep_sort_key order.

    Undoes what `gn format` does to lists holding commented-out entries: it
    hangs each comment block on the label that follows it (so the two travel
    together when the list is sorted) and prints a blank line in front of it.
    A trailing `# note` of an entry (e.g. "# peerdir only") stays with it, and
    is joined back into one line when `gn format` wrapped it past 80 columns
    onto a comment-only line aligned under it. A block whose lines are not all
    plain `"label",` entries is left untouched.
    """
    def fix(m):
        entries, item_indent, note_column = [], None, None
        for line in m.group("body").splitlines():
            if not line.strip():
                continue                      # blank line inserted by gn format
            e = _DEP_ENTRY_RE.match(line)
            if not e:
                cont = re.match(r"^(\s*)#\s*([^\"]*?)\s*$", line)
                if cont and entries and entries[-1][2] and len(cont.group(1)) == note_column:
                    c, label, note = entries[-1]  # wrapped tail of the previous note
                    entries[-1] = (c, label, note.rstrip() + " " + cont.group(2))
                    continue
                return m.group(0)             # not a plain label list: hands off
            if item_indent is None:
                item_indent = e.group(1)      # keep the file's own indentation
            entries.append((bool(e.group(2)), e.group(3), e.group(4)))
            note_column = e.start(4) if e.group(4) else None
        if not entries:
            return m.group(0)
        body = "".join('%s%s"%s",%s\n' % (item_indent, "# " if c else "", label,
                                           "  " + note.strip() if note else "")
                       for c, label, note in sorted(entries, key=lambda e: dep_sort_key(e[1])))
        return "%s%s = [\n%s%s]" % (m.group("indent"), m.group("key"),
                                    body, m.group("indent"))
    return _DEP_BLOCK_RE.sub(fix, text)


def shorten(label, host):
    """A label of a target declared in the host's own BUILD.gn in its short
    local form: "//host:name" -> ":name", and "//host" (the implicit
    basename(host) target) -> ":basename"."""
    if label == "//" + host:
        return ":" + os.path.basename(host)
    prefix = "//%s:" % host
    if label.startswith(prefix):
        return label[len("//" + host):]   # -> ":name"
    return label


def _gn_string(value):
    return '"%s"' % value.replace("\\", "\\\\").replace('"', '\\"').replace("$", "\\$")


def _resource_path_literal(ref, host):
    kind, rel = ref
    if kind == "gen":
        return '"$root_gen_dir/' + _gn_string(rel)[1:]
    if rel == host or rel.startswith(host + "/"):
        return _gn_string(os.path.relpath(rel, host))
    return _gn_string("//" + rel)


def _resource_files_scope(group, host):
    """A `resource_files` scope for a RESOURCE_FILES group, or None when the
    template can't derive ya's keys from it: every file must be a source file
    keyed resfs/file/<prefix><path relative to base_dir>, with base_dir either
    the module dir or the source root."""
    for base in (group.moddir, ""):
        files = []
        for (kind, rel), rootrel, key in group.entries:
            name = os.path.relpath(rel, base) if base else rel
            if (kind != "src" or rootrel != rel or name.startswith("..")
                    or key != "resfs/file/" + group.prefix + name):
                files = None
                break
            files.append(_resource_path_literal((kind, rel), host))
        if files is None:
            continue
        scope = []
        if group.prefix:
            scope.append("prefix = %s" % _gn_string(group.prefix))
        if not base:
            scope.append('base_dir = "//"')
        elif os.path.normpath(base) != os.path.normpath(host):
            scope.append("base_dir = %s" % _gn_string(os.path.relpath(base, host)))
        return scope + ["files = ["] + ["    %s," % f for f in files] + ["]"]
    return None


def render_resources(items, host):
    """Scopes for library()'s `resources` and `resource_files` (see
    build/gn/resources.gni), each a list of `name = value` lines. A
    RESOURCE_FILES group that _resource_files_scope can't express (DEST/STRIP,
    generated files) is spelled out as explicit `resources` entries."""
    resources, resource_files = [], []
    for item in items:
        if item[0] == "text":
            resources.append(["key = %s" % _gn_string(item[1]),
                              "value = %s" % _gn_string(item[2])])
        elif item[0] == "file":
            resources.append(["path = %s" % _resource_path_literal(item[1], host),
                              "key = %s" % _gn_string(item[2])])
        elif item[1].entries:
            scope = _resource_files_scope(item[1], host)
            if scope is not None:
                resource_files.append(scope)
                continue
            for ref, rootrel, key in item[1].entries:
                resources.append(["key = %s" % _gn_string("resfs/src/" + key),
                                  "value = %s" % _gn_string(rootrel)])
                resources.append(["path = %s" % _resource_path_literal(ref, host),
                                  "key = %s" % _gn_string(key)])
    return resources, resource_files


def _render_target(tmpl, name, public_deps, deps, sources,
                   serialize_enum_headers=(), commented=frozenset(),
                   resources=(), resource_files=(), notes=None, link_slot_provides=(),
                   link_slot_interface=None, link_slot_headers=(), link_slot_flags_from=(),
                   link_slot_module_interfaces=(), yql_abi_version=None, configs=(), public_configs=(), extra_plugins=()):
    """`notes` maps a dep label to a trailing comment ("peerdir only")."""
    lines = ['%s("%s") {' % (tmpl, name)]
    notes = notes or {}

    def block(key, items):
        if not items:
            return
        lines.append("    %s = [" % key)
        for it in items:
            prefix = "# " if it in commented else ""
            suffix = "  # " + notes[it] if it in notes else ""
            lines.append('        %s"%s",%s' % (prefix, it, suffix))
        lines.append("    ]")
        lines.append("")

    block("public_deps", public_deps)
    block("deps", deps)
    block("sources", sources)
    block("serialize_enum_headers", serialize_enum_headers)
    # link slot providers and the UDF ABI are handled by the templates: see
    # library() in //build/gn/base.gni, contrib_library() in
    # //build/gn/contrib.gni and linked_executable() in
    # //build/gn/link_slots.gni. A group() compiles nothing. The current UDF ABI
    # is a default config of every target; only an explicit YQL_ABI_VERSION() is
    # spelled out.
    if yql_abi_version == "current":
        yql_abi_version = None
    if tmpl in ("library", "contrib_library"):
        block("link_slot_provides", link_slot_provides)
    if tmpl in ("library", SLOT_INTERFACE_GROUP) and link_slot_interface:
        lines.append('    link_slot_interface = "%s"' % link_slot_interface)
        lines.append("")
        block("link_slot_headers", link_slot_headers)
        block("link_slot_flags_from", link_slot_flags_from)
    if tmpl == "library":
        block("link_slot_module_interfaces", link_slot_module_interfaces)
    if tmpl in ("library", "linked_executable") and yql_abi_version is not None:
        lines.append('    yql_abi_version = "%s"' % yql_abi_version)
        lines.append("")
    def scopes(key, items):
        # items: scopes as lists of ready `name = value` lines (see render_resources)
        if not items:
            return
        lines.append("    %s = [" % key)
        for scope in items:
            lines.append("        {")
            lines.extend("            " + l for l in scope)
            lines.append("        },")
        lines.append("    ]")
        lines.append("")

    # CPP_PROTO_PLUGIN0: an extra protoc plugin, see //build/gn/protobuf.gni
    scopes("extra_plugins", [['name = "%s"' % name, 'target = "%s"' % label]
                             for name, label in extra_plugins])
    scopes("resources", resources)
    scopes("resource_files", resource_files)
    if configs:
        # last in the target. Every template the generator emits (library,
        # contrib_library, protobuf_library, linked_executable) appends the
        # invoker's configs to the defaults itself, so this is a plain `=`.
        lines.append("    configs = [")
        lines.extend('        "%s",' % c for c in configs)
        lines.append("    ]")
        lines.append("")
    if public_configs:
        lines.append("    public_configs = [")
        lines.extend('        "%s",' % c for c in public_configs)
        lines.append("    ]")
        lines.append("")
    if lines[-1] == "":
        lines.pop()
    lines.append("}")
    return "\n".join(lines)


def render_spec(spec, host, remap, uncommented, main_name=None, private_configs=None):
    """`main_name`: for a GN_DIRECTIVES part, the rendered name of its module's
    main target."""
    tmpl = {"PROGRAM": "linked_executable", "GROUP": "group",
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

    # an executable has no `resources`: a PROGRAM's RESOURCE()s go to a sibling
    # library("<name>_resources") the executable depends on (its registration
    # code runs when that library is loaded).
    resources, resource_files = render_resources(spec.resources, host)
    plugins = [(name, module_label(d) if os.path.basename(d) == name
                else "%s:%s" % (module_label(d), name))
               for name, d in spec.proto_plugins]
    resources_target = "%s_resources" % spec.name
    split_resources = tmpl == "linked_executable" and bool(spec.resources)

    pub = {shorten(remap.get(l, l), host) for l in spec.public_deps}
    dep = {shorten(remap.get(l, l), host) for l in spec.deps} - pub

    # deps vs public_deps is always the freshly computed classification
    # (finalize_publicity); only the comment state comes from the existing file.

    if split_proto:
        dep.add(":private_proto")
    if split_resources:
        dep.add(":" + resources_target)

    pub = sorted(pub, key=dep_sort_key)
    dep = sorted(dep, key=dep_sort_key)
    main_sources = spec.sources + spec.proto_sources if tmpl == "protobuf_library" else spec.sources
    srcs = sorted(os.path.relpath(s, host) if s.startswith(host + "/") else "//" + s
                  for s in main_sources)
    enums = sorted(os.path.relpath(h, host) if h == host or h.startswith(host + "/")
                   else "//" + h for h in spec.enum_headers)

    # A LIBRARY() / PROTO_LIBRARY() whose SRCS hold no compilable files (e.g.
    # only a header, or none at all) is a facade that just re-exports its
    # PEERDIRs. GN has no empty static library and protobuf_library requires
    # sources; emit a group() that forwards the deps instead.
    if (tmpl in ("library", "contrib_library", "protobuf_library") and not srcs
            and not split_proto and not enums and not spec.resources):
        tmpl = "group"

    # New deps default to commented-out: a freshly derived dependency is emitted
    # disabled so it can be re-enabled by hand (otherwise it is redundant and
    # closes the dep cycles GN forbids). Only the user's own choices survive a
    # regen -- a label left UNcommented in the existing file stays uncommented;
    # everything else (new, or previously commented) is re-emitted commented.
    # group() plays by the same rules as library(), whether it is a sourceless
    # facade or a pure aggregator: nothing about forwarding deps instead of
    # linking them makes a freshly derived edge more trustworthy. Left intact:
    # the protobuf_library sub-target (its deps are proto imports the generated
    # .pb.h genuinely needs) and a library's own :private_proto edge (an
    # internal artifact, never a cycle).
    # an existing file may spell a local label long ("//host"): compare in
    # the short form the labels are rendered in
    # a PROGRAM(<name>) target may still carry its directory's name in the file
    existing_name = spec.name if spec.name in uncommented else os.path.basename(spec.dir)
    keep = {shorten(remap.get(l, l), host) for l in uncommented.get(existing_name, set())}
    render = lambda labels: {shorten(remap.get(l, l), host) for l in labels}
    if spec.part is not None and spec.name not in uncommented:
        # a part new to this file: its sources lived in the module's main
        # target so far, and so did the user's choices about their deps
        keep |= {shorten(remap.get(l, l), host) for l in uncommented.get(main_name, set())}
    # a dep on a module left enabled carries over to a dep on its GN_DIRECTIVES
    # part: the narrower edge of the same kind (the module's headers moved there)
    for l in spec.deps | spec.public_deps:
        d, _, p = l[2:].partition(":")
        if l.startswith("//") and p and not d.startswith("contrib/"):
            if render([module_label(d)]) <= keep:
                keep |= render([l])
    # likewise to a child folded into that module's BUILD.gn, when the edge to
    # the module itself is gone: the headers it was derived from now belong to
    # the child (see Resolver.claims) -- so not an edge no header is behind
    derived = render(spec.deps | spec.public_deps)
    for l in (spec.deps | spec.public_deps) - spec.peerdir_only:
        d, _, p = remap.get(l, l)[2:].partition(":")
        if p and l != remap.get(l, l) and not d.startswith("contrib/"):
            parent = render([module_label(d)])
            if parent <= keep and not parent <= derived:
                keep |= render([l])
    peerdir_only = (render(spec.peerdir_only)
                    - render((spec.deps | spec.public_deps) - spec.peerdir_only))
    notes = {l: "peerdir only" for l in peerdir_only}

    # GN_DIRECTIVES deps are written by hand in ya.make (including the main
    # target's edges to its own parts, which in ya are the module itself):
    # never optional, never commented
    own_parts = render(spec.explicit)

    commented = frozenset()
    if tmpl in ("library", "contrib_library", "linked_executable", "group"):
        commented = frozenset(l for l in pub + dep
                              if l not in keep and l not in own_parts
                              and l not in (":private_proto", ":" + resources_target))
    elif tmpl == "protobuf_library":
        # proto imports are genuinely needed and stay enabled; a PEERDIR with
        # no import behind is new to this target and starts disabled as usual
        commented = frozenset(l for l in peerdir_only if l not in keep)

    # Hand-written config("<target>_private_config") / ("<target>_public_config")
    # of the existing file (compiler flags GN can't derive from ya.make) are
    # kept and pulled in.
    def carried_config(suffix):
        config_name = spec.name + suffix
        config_text = (private_configs or {}).get(config_name)
        if config_text is None and spec.part is None:
            # the same config under the directory's name (a PROGRAM(<name>) target
            # renamed from it): carried over under the target's name
            old = os.path.basename(spec.dir) + suffix
            if old != config_name and old in (private_configs or {}):
                config_text = re.sub(r'config\(\s*"%s"' % re.escape(old), 'config("%s"' % config_name,
                                     private_configs[old], count=1)
        return config_name, config_text

    config_name, config_text = carried_config(PRIVATE_CONFIG_SUFFIX)
    configs = [":" + config_name] if config_text is not None and tmpl != "group" else ()
    public_config_name, public_config_text = carried_config(PUBLIC_CONFIG_SUFFIX)
    public_configs = [":" + public_config_name] if public_config_text is not None else ()

    out = []
    if config_text is not None:
        out.append(config_text)
    if public_config_text is not None:
        out.append(public_config_text)
    if split_proto:
        proto_pub = sorted({shorten(remap.get(l, l), host) for l in spec.proto_public},
                           key=dep_sort_key)
        proto_srcs = sorted(os.path.relpath(s, host) for s in spec.proto_sources)
        out.append(_render_target("protobuf_library", "private_proto", proto_pub, [], proto_srcs,
                                  extra_plugins=plugins))
    if split_resources:
        out.append(_render_target("library", resources_target, [], [], [],
                                  resources=resources, resource_files=resource_files))
        resources, resource_files = (), ()
    # a header-only interface of link slots: a group() that makes the weak
    # references, borrowing the compiler flags of a source that is compiled
    # with the headers (by a variant of the module: the slot's providers)
    slot_flags_from = ()
    if spec.slot_headers and tmpl == "group":
        tmpl = SLOT_INTERFACE_GROUP
        if spec.slot_flags_from:
            slot_flags_from = [os.path.relpath(spec.slot_flags_from, host)]
    out.append(_render_target(tmpl, spec.name, pub, dep, srcs, enums, commented=commented,
                              configs=configs, public_configs=public_configs,
                              resources=resources, resource_files=resource_files,
                              notes=notes, extra_plugins=plugins if tmpl == "protobuf_library" else (),
                              link_slot_provides=spec.slot_provides,
                              link_slot_interface=spec.slot_interface,
                              link_slot_headers=[_slot_header(h, host) for h in spec.slot_headers],
                              link_slot_flags_from=slot_flags_from,
                              link_slot_module_interfaces=spec.slot_module_interfaces,
                              yql_abi_version=spec.yql_abi if tmpl != "protobuf_library" else None))
    return "\n\n".join(out)


# the template (//build/gn/link_slots.gni) of a group() that is a link slot interface
SLOT_INTERFACE_GROUP = "slot_interface_group"


def _slot_header(h, host):
    """A repo-relative header as written in the BUILD.gn of `host`."""
    return os.path.relpath(h, host) if h.startswith(host + "/") else "//" + h


def _absolute_labels(labels, host):
    return {"//%s%s" % (host, l) if l.startswith(":") else l for l in labels}


def render_file(root, host, specs, remap, inherit=None):
    """`inherit`: {module dir: {part or None: labels}} -- a module whose targets
    move out of its parent's file (see plan_merge): the deps left uncommented
    in its old targets there."""
    path = os.path.join(root, host, "BUILD.gn")
    uncommented = {n: _absolute_labels(ls, host)
                   for n, ls in parse_existing_buildgn_uncommented(path).items()}
    for s in specs:
        labels = (inherit or {}).get(s.dir, {}).get(s.part)
        if s.name not in uncommented and labels is not None:
            uncommented[s.name] = labels
        # the SELF_PART of a module split by an earlier run and not now: its
        # deps are the module's own target's again
        if (s.part is None and s.dir == host and SELF_PART in uncommented
                and s.name != SELF_PART and not any(t.part == SELF_PART for t in s.parts)):
            uncommented[s.name] = uncommented.get(s.name, set()) | uncommented[SELF_PART]
    # hand-written private configs: from this file, and from the file a target
    # is moving here from (Pass 2 merge), where it was written
    private_configs = {}
    for s in specs:
        if s.dir != host:
            private_configs.update(parse_existing_configs(os.path.join(root, s.dir, "BUILD.gn")))
    private_configs.update(parse_existing_configs(path))
    main_names = {id(part): s.name for s in specs for part in s.parts}
    return normalize_dep_lists(
        "\n\n".join(render_spec(s, host, remap, uncommented, main_names.get(id(s)),
                                private_configs)
                    for s in sorted(specs, key=lambda s: s.name)) + "\n")


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
        self.aliased = []
        self.self_split = []           # (dir, {modules it PEERDIRs that include it})
        self.merged_groups = []        # (dir, [modules it absorbs]), plan_merge_groups
        self.unmerged = []             # (dir, parent): split modules kept out of the parent's file
        self.resources_missing = defaultdict(set)
        self.generated = []
        self.deleted = []
        self.preserved = []
        self.yql_abi_explicit = []     # (dir, "M.m.p")
        self.yql_abi_missing = []      # dirs including //yql/essentials/public/udf without YQL_*ABI_VERSION
        self.provides_no_slot = []     # (dir, PROVIDES name): no link slot interface anywhere
        self.unknown_srcs = []         # (dir, SRCS of no kind the generator builds: dropped)

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
        if self.merged_groups:
            print("\nmerged groups (in-directory cycles; the modules are compiled by the first):", file=o)
            for d, members in self.merged_groups:
                print("  //%s <- %s" % (d, ", ".join(members)), file=o)
        if self.self_split:
            print("\nanti-cycle facades (`# gn: %s`; <- the modules it PEERDIRs that include it):"
                  % ANTI_CYCLE_FACADE, file=o)
            for d, cs in self.self_split:
                print("  //%s <- %s" % (d, ", ".join(sorted(cs)) or "NONE: the directive is stale"), file=o)
        if self.unmerged:
            print("\nnot merged into the parent (split into a facade + :%s):" % SELF_PART, file=o)
            for c, p in self.unmerged:
                print("  //%s (parent //%s)" % (c, p), file=o)
        if self.aliased:
            print("\ninclude-side modules folded into their src module (INCLUDE_ROOT_REMAP):", file=o)
            for d, t in sorted(set(self.aliased)):
                print("  //%s -> %s" % (d, t if t.startswith("//") else "//" + t), file=o)
        if self.keep_collapsed:
            print("\nfolded into external parent (child not generated, dep -> folded label):", file=o)
            for c, label in sorted(self.keep_collapsed):
                print("  //%s -> %s" % (c, label), file=o)
        if self.skipped:
            print("\nskipped (non-trivial):", file=o)
            for d, reasons in sorted(self.skipped):
                print("  %-58s %s" % (d, ",".join(sorted(set(reasons)))), file=o)
        if self.yql_abi_explicit:
            print("\nyql abi explicit (YQL_ABI_VERSION):", file=o)
            for d, v in sorted(set(self.yql_abi_explicit)):
                print("  %-58s %s" % (d, v), file=o)
        if self.yql_abi_missing:
            print("\nincludes udf without yql abi (ya make would #error; gn builds it with the current ABI):", file=o)
            for d in sorted(set(self.yql_abi_missing)):
                print("  %s" % d, file=o)
        if self.provides_no_slot:
            print("\nPROVIDES of no link slot (no `# gn: slot` interface anywhere; ya's check only):", file=o)
            for d, name in sorted(set(self.provides_no_slot)):
                print("  %-58s %s" % (d, name), file=o)
        if self.unknown_srcs:
            print("\nSRCS of no known kind (neither C++, a header, .proto nor TRANSLATED_EXTS; not built):", file=o)
            for d, srcs in sorted(self.unknown_srcs):
                print("  %-58s %s" % (d, " ".join(srcs)), file=o)
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
        if self.resources_missing:
            print("\nresource inputs not found in the source tree (generated? emitted as is):", file=o)
            for d in sorted(self.resources_missing):
                for p in sorted(self.resources_missing[d]):
                    print("  [%s] %s" % (d, p), file=o)
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


# --- variant sources ---------------------------------------------------------
#
# ya make builds one module in one configuration, so a module wanted twice
# (with and without LLVM codegen: minikql/computation/{llvm16,no_llvm}, ...)
# is two modules INCLUDE()-ing a shared ya.make.inc that names the sources
# (SET(ORIG_SRC_DIR ...) + SET(ORIG_SOURCES ...)) and COPY()-es them into
# each module's build dir. GN compiles one source in any number of targets,
# so the variants are instances of one hand-written template; the generator
# only keeps the source list in sync: <dir of ya.make.inc>/variant_sources.gni.
VARIANT_SOURCES_GNI = "variant_sources.gni"
VARIANT_GNI = "variant.gni"     # the family's template, hand-written


def find_variant_incs(root, starts):
    for start in starts:
        for cur, dirs, files in os.walk(os.path.join(root, start)):
            rel = os.path.relpath(cur, root)
            if rel.split(os.sep)[0] == "contrib":
                dirs[:] = []
                continue
            if "ya.make.inc" in files:
                yield os.path.join(rel, "ya.make.inc")


def variant_inc(root, d):
    """The ya.make.inc the ya.make of module d INCLUDEs (see _is_variant),
    repo-relative, or None."""
    try:
        with open(os.path.join(root, d, "ya.make"), encoding="utf-8", errors="ignore") as f:
            m = re.search(r"INCLUDE\(\s*([^)\s]*ya\.make\.inc)\s*\)", f.read())
    except OSError:
        return None
    if m is None:
        return None
    inc = m.group(1)
    for prefix in ("${ARCADIA_ROOT}/", "//"):
        if inc.startswith(prefix):
            return os.path.normpath(inc[len(prefix):])
    return os.path.normpath(os.path.join(d, inc))


def render_variant_sources(root, inc):
    """The variant_sources.gni text for ya.make.inc `inc`, or None if it does
    not name its sources with SET(ORIG_SOURCES)."""
    with open(os.path.join(root, inc), encoding="utf-8") as f:
        text = resolve_conditionals(strip_yamake_comments(f.read()))
    values = {}
    for name, args, _ in scan_macros(text):
        if name == "SET" and args and args[0] in ("ORIG_SRC_DIR", "ORIG_SOURCES"):
            values[args[0]] = args[1:]
    if "ORIG_SOURCES" not in values:
        return None
    src_dir = (values.get("ORIG_SRC_DIR") or [os.path.dirname(inc)])[0]
    src_dir = src_dir.replace("${ARCADIA_ROOT}/", "")
    sources = sorted("//" + os.path.normpath(os.path.join(src_dir, s))
                     for s in values["ORIG_SOURCES"] if s.endswith(COMPILED_EXTS))
    lines = ["# Generated by yamake2gn from ya.make.inc (SET(ORIG_SOURCES)); do not edit.",
             "", "variant_sources = ["]
    lines += ['  "%s",' % s for s in sources]
    lines += ["]", ""]
    return "\n".join(lines)


def find_contrib_modules(root):
    out = []
    for dirpath, dirnames, filenames in os.walk(os.path.join(root, "contrib")):
        if "ya.make" in filenames:
            out.append(os.path.relpath(dirpath, root))
    return sorted(out)


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
                         "dependency closure reached from them (via #include graph "
                         "and PEERDIR)")
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

    external = ExternalTargets(root)
    resolver = Resolver(root, mods, external)
    merge_aliases(mods, resolver)
    link_foreign_sources(mods, resolver)
    report = Report()
    drop_slotless_provides(root, mods, report)
    specs_by_dir = {}

    # The whole tree's #include graph: which files belong to each module and
    # which headers are included across module boundaries. Built over every
    # real module, not only the ones this run generates.
    graph = IncludeGraph(resolver).build(mods, real_dirs)
    cycles = CycleGraph(mods, resolver, graph, real_dirs)
    groups = plan_merge_groups(mods, resolver, cycles, external)
    for p, members in sorted(groups.items()):
        for c in sorted(members):
            absorb(mods, resolver, c, p)
    report.merged_groups = sorted((p, sorted(m)) for p, m in groups.items())
    if groups:
        # the absorbed modules' files are the absorbing ones' now (what the
        # files include does not change)
        regraph = IncludeGraph(resolver)
        regraph._includes = graph._includes
        graph = regraph.build(mods, real_dirs)
        cycles = CycleGraph(mods, resolver, graph, real_dirs)
    resolver.cycles = cycles
    resolver.self_split = plan_self_split(mods, cycles)
    report.self_split = sorted(resolver.self_split.items())

    def gen_one(d):
        if d in resolver.aliases:
            # include-side half of a module whose GN target is the src one
            report.aliased.append((d, module_part_label(resolver.aliases[d], resolver.alias_parts.get(d))))
            return None
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
        if mod.absorbed_into is not None:
            return None     # compiled by another module's target; no target of its own
        if not is_trivial(mod):
            report.skipped.append((d, nontrivial_macros(mod) or ["<no module>"]))
            return None
        s = gen_spec(mod, resolver, graph, report, external)
        specs_by_dir[d] = s
        unknown = [x for x in mod.compiled_srcs()
                   if not x.endswith(COMPILED_EXTS + HEADER_EXTS + PROTO_EXTS)]
        if unknown:
            report.unknown_srcs.append((d, unknown))
        if mod.yql_abi not in (None, "current"):
            report.yql_abi_explicit.append((d, mod.yql_abi))
        elif (mod.yql_abi is None and mod.kind != "PROTO_LIBRARY"
                and UDF_MODULE in graph.owners(d)):
            report.yql_abi_missing.append(d)
        return s

    if args.as_target:
        # ---- Pass 1: transitive closure over the derived #include graph -------
        slots = link_slot_providers(root, mods)
        queue = []
        for p in args.paths:
            md = find_module_dir(root, os.path.relpath(os.path.abspath(p), root))
            if md is None:
                ap.error("no ya.make at or above %s" % p)
            queue.append(md)
        seen = set()
        while queue:
            d = resolver.alias(queue.pop())
            if d in seen or d not in mods or not is_real(mods[d]):
                seen.add(d)
                continue
            seen.add(d)
            s = gen_one(d)
            if s is None and mods[d].absorbed_into is not None:
                continue
            if s is None:
                # Module not generated here (keep-marked or non-trivial): follow
                # the owners of what its files include, exactly as for a
                # generated module -- the #include graph covers every real
                # module. Its BUILD.gn may spell a dep as a folded //P:name, or
                # not list it at all; the includes don't depend on that.
                queue.extend(sorted(graph.owners(d)))
                # ... and its PEERDIRs, like a generated module's peerdir deps
                queue.extend(p for p in peerdir_owners(mods[d], resolver)
                             if not p.startswith("contrib/"))
                # ... and its protoc plugins (CPP_PROTO_PLUGIN0): extra_plugins,
                # not deps, yet built for it all the same
                queue.extend(pd for _, pd in mods[d].proto_plugins)
                # Plus the deps of its hand-written BUILD.gn: link-only deps
                # added by hand (e.g. //ydb/core/graph/service behind an
                # include of ydb/core/graph/api/service.h) have no include.
                existing_gn = os.path.join(root, d, "BUILD.gn")
                if os.path.exists(existing_gn):
                    # ... and the providers a linked_executable() selects
                    with open(existing_gn, encoding="utf-8") as f:
                        selects = re.findall(r'link_select\s*=\s*\[(.*?)\]', f.read(), re.S)
                    for entry in re.findall(r'"([^"]+)"', "".join(selects)):
                        slot, _, choice = entry.partition("=")
                        queue.extend(sorted(slots.get((slot, choice), ())))
                    existing = parse_existing_buildgn(existing_gn)
                    for _deps, _pub in existing.values():
                        for label in _deps | _pub:
                            if not label.startswith("//"):
                                continue    # ":local" -- a target of this same file
                            dep = label[2:].split(":")[0]
                            if not dep.startswith("contrib/"):
                                queue.append(dep)
                continue
            for label in set().union(*[t.deps | t.public_deps for t in [s] + s.parts]):
                dep = label[2:].split(":")[0]   # //dir or //dir:name -> dir
                if dep.startswith("contrib/"):
                    continue                    # external: keeps its own BUILD.gn
                queue.append(dep)
            queue.extend(pd for _, pd in s.proto_plugins)   # extra_plugins
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
    all_specs = specs + [part for s in specs for part in s.parts]

    # publicity is a graph property: resolve it once the consumer set is known
    # (headers another GN_DIRECTIVES part of the same module includes count too)
    local_consumed = set().union(*[t.local_consumed for t in all_specs])
    finalize_publicity(all_specs, graph.consumed | local_consumed)

    # ---- Pass 2: merge, strictly over what Pass 1 generated ------------------
    remap, absorbed, dropped, unmerged = plan_merge(specs, report)
    # a child folded into its parent's file by an earlier run and not now (a
    # split module, see plan_merge, or a merge blocked since): its old
    # //P:<name> is its //P/<child> now, and the user's choices about its deps
    # are read from the parent's file
    inherit = {}
    for s in specs:
        c, P = s.dir, os.path.dirname(s.dir)
        parent_gn = os.path.join(root, P, "BUILD.gn")
        if s.host != c or not P or external.is_external(P) or not os.path.exists(parent_gn):
            continue
        existing = parse_existing_buildgn_uncommented(parent_gn)
        own = ({t.name for t in [specs_by_dir[P]] + specs_by_dir[P].parts}
               if P in specs_by_dir else set())
        own |= {t.name for t in all_specs if t.host == P}
        b = os.path.basename(c)
        names = {}
        for part, old in ((None, [b, "%s_%s" % (os.path.basename(P), b)]),
                          (SELF_PART, ["%s_%s" % (b, SELF_PART)])):
            old = [n for n in old if n in existing and n not in own]
            for n in old:
                remap.setdefault("//%s:%s" % (P, n), module_part_label(c, part))
            if old:
                # read now: the parent's file is rewritten before the child's
                names[part] = set().union(*[_absolute_labels(existing[n], P) for n in old])
        if names:
            inherit[c] = names
    # ... and the other way round: a child folded into its parent's file now
    # and not by an earlier run: read from its own file (removed then)
    for s in specs:
        own_gn = os.path.join(root, s.dir, "BUILD.gn")
        if s.host == s.dir or not os.path.exists(own_gn):
            continue
        existing = parse_existing_buildgn_uncommented(own_gn)
        for t in [s] + s.parts:
            old = [n for n in ([os.path.basename(s.dir), SELF_PART] if t.part is None else [t.part])
                   if n in existing]
            if old:
                # (the facade's own edge to its SELF_PART is no choice)
                inherit.setdefault(s.dir, {})[t.part] = (
                    set().union(*[_absolute_labels(existing[n], s.dir) for n in old])
                    - {module_part_label(s.dir, SELF_PART)})
    # PROGRAM(<name>) not named after its directory: //<dir> is //<dir>:<name>
    # (a merged one was remapped to //<parent>:<name> by plan_merge already)
    for s in specs:
        if s.dir not in absorbed and s.name != os.path.basename(s.dir):
            remap.setdefault(module_label(s.dir), "//%s:%s" % (s.dir, s.name))

    # Generated leaf children whose collapsed target a keep/contrib parent
    # already provides: drop them and remap //child -> //parent:child. Decided
    # here (not Pass 1) so the children were generated honestly and only the
    # geometry that permits a normal merge permits this fold too.
    ext_remap, collapsed = plan_external_collapse(specs, external, report)
    remap.update(ext_remap)
    # likewise a child not generated at all (non-trivial: SRC_C_AVX2, ...) that
    # its hand-written parent builds
    for d in sorted(real_dirs - set(specs_by_dir)):
        P = os.path.dirname(d)
        if P and external.is_external(P) and external.declares(P, os.path.basename(d)):
            remap.setdefault(module_label(d), "//%s:%s" % (P, os.path.basename(d)))
    # a dep on a module absorbed by another (`# gn: into`) is a dep on that one
    for d, into in resolver.absorbed.items():
        remap[module_label(d)] = remap.get(module_label(into), module_label(into))
    for d, m in mods.items():
        if m.absorbed_into is not None and d not in resolver.absorbed:   # into a part
            remap.setdefault(module_label(d), m.absorbed_into)
    # an existing file may still spell the SELF_PART of a module split by an
    # earlier run: the module's own target is what it is now
    for d in real_dirs:
        if d not in resolver.self_split:
            remap.setdefault(module_part_label(d, SELF_PART),
                             remap.get(module_label(d), module_label(d)))
    # an existing file may still spell a folded contrib target by its dir
    # (//contrib/libs/brotli/c/dec): read its comment state under the new label
    for d in find_contrib_modules(root):
        label = external.label(d)
        if label is not None and label != module_label(d):
            remap.setdefault(module_label(d), label)

    # Pure aggregators that were NOT merged: keep the bundle as a group() of its
    # direct real children (their PEERDIR-style semantics), instead of an empty
    # library. "Pure" means no SRCS of its own: deps derived from the dir's
    # headers that consumers include (IncludeGraph) don't make it a library,
    # they are forwarded alongside the children. Not the variants of a shared
    # ya.make.inc (llvm16 / no_llvm): they are alternatives, never both; one
    # is picked by a PEERDIR on it or by a link slot of the executable
    # (minikql_codegen, yt_codegen: build/gn/link_slots.gni).
    children_map = defaultdict(list)
    for d in real_dirs:
        if not _is_variant(root, d):
            children_map[os.path.dirname(d)].append(d)
    for s in specs:
        if (s.dir not in dropped and not s.has_srcs
                and not s.enum_headers and not s.resources
                and children_map.get(s.dir)):
            s.kind = "GROUP"
            s.public_deps |= {module_label(c) for c in children_map[s.dir]}

    host_specs = defaultdict(list)
    for s in specs:
        if s.dir in dropped and s.host == s.dir:
            continue  # aggregator target dropped
        if s.dir in collapsed:
            continue  # folded into a keep/contrib parent; provided there
        host_specs[s.host].append(s)
        host_specs[s.host].extend(s.parts)

    # ---- emit ----------------------------------------------------------------
    for host, slist in sorted(host_specs.items()):
        text = render_file(root, host, slist, remap, inherit)
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
                # `gn format` reflows the dep lists around the commented-out
                # entries (blank line before each comment block, comment blocks
                # dragged along when sorting) -- put them back in shape. This
                # runs last on purpose, so the file on disk is the final word.
                with open(out, encoding="utf-8") as f:
                    formatted = f.read()
                normalized = normalize_dep_lists(formatted)
                if normalized != formatted:
                    with open(out, "w", encoding="utf-8") as f:
                        f.write(normalized)

    # the include-side half's own BUILD.gn is stale once its src module is written
    stale_aliases = {d for d, t in resolver.aliases.items()
                     if t in specs_by_dir and not external.is_external(d)}
    report.aliased += [(d, module_part_label(resolver.aliases[d], resolver.alias_parts.get(d)))
                       for d in stale_aliases]
    # a module compiled by another one's target has no target, nor file
    no_target = {d for d, m in mods.items()
                 if m.absorbed_into is not None and d not in host_specs
                 and (m.absorbed_into[2:].partition(":")[0] in specs_by_dir
                      or external.is_external(m.absorbed_into[2:].partition(":")[0]))
                 and not external.is_external(d)}
    for c in sorted(absorbed | collapsed | stale_aliases | no_target):
        old = os.path.join(root, c, "BUILD.gn")
        if os.path.exists(old):
            report.deleted.append(c)
            if args.dry_run:
                print("# DELETE %s/BUILD.gn (merged into %s)"
                      % (c, resolver.aliases.get(c, os.path.dirname(c))))
            else:
                os.remove(old)

    # the source lists of variant modules (see VARIANT_SOURCES_GNI): with
    # --as-target, of the variants the closure reached, wherever their
    # ya.make.inc lies
    if args.as_target:
        incs = {variant_inc(root, d) for d in seen} - {None}
    else:
        inc_roots = ([os.path.relpath(os.path.abspath(p), root) for p in args.paths]
                     if args.paths else ["."])
        incs = set(find_variant_incs(root, inc_roots))
    for inc in sorted(incs):
        # only for a family built by a template of its own (variant.gni)
        if not os.path.exists(os.path.join(root, os.path.dirname(inc), VARIANT_GNI)):
            continue
        text = render_variant_sources(root, inc)
        if text is None:
            continue
        out = os.path.join(root, os.path.dirname(inc), VARIANT_SOURCES_GNI)
        if args.dry_run:
            print("# ----- %s -----\n%s" % (os.path.relpath(out, root), text))
        else:
            with open(out, "w", encoding="utf-8") as f:
                f.write(text)

    report.dump()


if __name__ == "__main__":
    main()
