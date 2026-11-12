#!/usr/bin/env python3
"""Moves the GN branch along the upstream branch one commit at a time (the front
pipeline of the GN follow-up; the branch is --branch, main by default): for
every first-parent commit U of the upstream after the last one merged

  1. merge     git merge U (rerere on); a conflict left -> STOP conflict
  2. review    U touches what the generator does not carry over: contrib
               (ya.make, *.inc, added/deleted sources), a module of a
               hand-written BUILD.gn (`yamake2gn: keep`), the ya build
               configuration (build/conf, ymake.core.conf, ...) -> STOP review: a
               `gn-follow: REVIEW` marker line goes to the top of the BUILD.gn
               to mirror it into (else build/gn/follow/REVIEW.md); the step
               goes on once all the markers are deleted
  3. regen     yamake2gn.py; a new item in the `skipped` or an ERROR section of
               its report against the accepted ones (build/gn/follow/report.txt,
               in the branch) -> STOP generator: the new items go into that
               file between <<<<<<< and >>>>>>> lines; fix them, or accept them
               by deleting those lines
  4. gn gen    skipped when 3 changed nothing and no hand-written input is
               dirty; failed -> STOP gn-gen
  5. build     autofix.py: ninja, uncommenting the edges it can; still
               failing -> STOP build
  6. commit    `gn: sync <U>` with what 3-5 changed (empty if nothing changed)
  7. bench     with --bench (off by default; the bench is --bench-root):
               follow_bench.py measures the incremental
               build of the commit by ninja and by ya make in the bench work
               tree, one after another (the order alternates): ninja --targets
               (all tests), ya --ya-dirs (ydbd, ydb) and what follow_select.py
               picks for U, after both build them in the previous commit, not
               measured (follow_bench.py --warm: ninja normally has nothing to
               do there, ya fills its cache with its targets); a failed build
               -> STOP bench (a finding: not fixed there; --run --bench
               measures again, --run --skip-bench adds a GN-Bench-Skip trailer
               to the commit and goes on without its measurement)
  8. amend     the GN-Bench-* trailers of the measurements go into the message
               of the commit (the tree stays the same)

A commit that touches only docs and .github skips 3-5, but still gets a sync
commit. With --only-head there is one step to the head of the upstream instead
of one per commit: one merge of the head, the review and the build over the
whole range since the last followed commit, one sync commit; the journal
records that step alone (with how many commits it skipped).
 Without a local journal, the latest first-parent merge named
`Merge <U>:` identifies the upstream commit (checked against its second parent),
and a following `gn: sync <U>:` completes its step. Manual fix commits may follow
either one. This restores the journal's starting point once; the pipeline then
uses the journal as usual, preserving local STOPs and bench progress. A restored
sync is not a local build: it still needs bootstrap on this machine.
After a STOP fix the work tree (or
finish the merge, delete the markers) and run again, like `git rebase
--continue`: the step resumes after the merge once the tree is in order.

Without a local journal, an unfinished follow merge resumes at review, then
regenerates and builds locally; changes by the generator are expected there.
Old steps without a sync commit are conservatively treated as unfinished.
Otherwise the first run checks the starting point: the generator on it must change
nothing (else STOP idempotence: commit what it changed, any message, or fix
the generator) and the build
must pass; with --bench it then builds the starting point in the bench
from scratch (warming the caches of the bench).

Steps run without --bench leave the bench tree behind; the next measured step
first moves it to the commit before its U (the warm-up then builds what it
missed, not measured).

The front build and the bench must not share a compiler cache: give the bench
a tag that is isolated in the config of dist-clangd (--bench-env
DC_TAGS=gn/follow/bench; with `distclang = true` in args.gn the builds are
tagged with the branch too). The build dirs may be inside the work trees
(e.g. <work tree>/out/gn): the generator and autofix.py skip them. autofix.py and follow_bench.py are taken
from the dir of this script, yamake2gn.py from the work tree.

Run it in the work tree of the GN branch (e.g. cd ~/g/ydb/gn-follow):

    build/gn/scripts/follow.py                               # the status of main
    build/gn/scripts/follow.py --branch stable-25.1          # the status of another branch
    build/gn/scripts/follow.py --max-commits 3               # the status, the 3 commits marked
    build/gn/scripts/follow.py --run --max-commits 3         # go on for 3 commits
    build/gn/scripts/follow.py --run --bench                 # go on, measuring every step
    build/gn/scripts/follow.py --run --skip-bench            # after a STOP bench: go on without it
    build/gn/scripts/follow.py --run --only-head             # one merge of the head, no per-commit steps

--branch NAME follows origin/NAME (main by default); --upstream REF overrides
it for a ref of another shape, e.g. a third-party remote. The first run takes
the bench and other settings, they are remembered in <state>/config.json (an
argument given later overrides and is saved):

    build/gn/scripts/follow.py --bench-root ~/g/ydb/gn-bench --ya-out ~/b/ya-bench/out \
        --ya-cache-root ~/b/ya-bench/cache --ya-cache-size 120GiB --bench-env DC_TAGS=gn/follow/bench

Defaults: the work tree is the one of the cwd, the build dir <root>/out/gn,
the state <git dir of the work tree>/gn-follow/<branch> (outside the work
tree, git does not see it; one state, config and bench per followed branch),
the bench build dir <bench root>/out/gn, nice 10.

The whole run is niced: `--nice N` raises the niceness of this process (default
10, 0 off), and every command it starts (the generator, gn gen, ninja, ya make)
inherits it.

Without --run it only shows the status and changes nothing: whether a
run is going on, the work tree, the upstream, where and why the last run
stopped and what to do, the last commits done with their measurements, the
pending commits with their properties (files, ya.make, what needs a review),
the bench tree. --run does the work: up to the upstream, or for --max-commits N
commits of it (the commit it stopped at counts, the bootstrap does not).

State dir: config.json (the remembered arguments), journal.jsonl (a record
per step; `mode: only-head` marks a step of --only-head), run.log (the output
of every run), report.txt (the accepted
generator report before it moved into the branch), blocked.txt (edges gn gen rejected, for
autofix.py), STOPPED.md (why the last run stopped), steps/<U>/ (logs of a
step, bench.json).

On a terminal a status line under the output shows the stage going on: ninja edges done/total,
failed and uncommented by autofix.py, the time left; in the bench its phase and ya's tasks
(from its event log). It is not in run.log; the lines of follow_bench.py are.

Exit code: 0 nothing left (or --max-commits done, or the status), 1 usage/precondition,
10 review, 11 conflict, 12 generator, 13 gn gen, 14 build, 15 idempotence,
16 bench.
"""

import argparse
import collections
import contextlib
import fcntl
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import threading
import sys
import time

SCRIPTS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPTS)
import follow_select  # noqa: E402

REPORT_FILE = "build/gn/follow/report.txt"       # the accepted problems of the generator report, in the branch
REVIEW_FILE = "build/gn/follow/REVIEW.md"       # review markers with no BUILD.gn to put them in
REVIEW_MARK = re.compile(r"^(# )?gn-follow: REVIEW ")
REPORT_NEW = "<<<<<<< new in "
REPORT_END = ">>>>>>>"
SKIP_TRAILER = "GN-Bench-Skip"
STOP_KINDS = {      # stage the run stopped at: (exit code, what to do after the STOP)
    "review": (10, "mirror the change by hand where the `gn-follow: REVIEW` markers are, delete them, "
                   "run again"),
    "conflict": (11, "finish the merge in the work tree, then run again"),
    "generator": (12, "fix the new problems, or accept them by deleting their marker lines in " + REPORT_FILE
                      + "; run again"),
    "gn-gen": (13, "fix the work tree (uncommitted), then run again"),
    "build": (14, "fix the work tree (uncommitted), then run again"),
    "idempotence": (15, "the generator changed the starting point: commit it (any message) or fix the "
                        "generator; run again"),
    "bench": (16, "a finding of the incremental build, not fixed in the bench tree: --run --bench measures "
                  "again, or --run --skip-bench to go on without the measurement of this commit"),
}
EXIT = {kind: code for kind, (code, _) in STOP_KINDS.items()}
HINTS = {kind: hint for kind, (_, hint) in STOP_KINDS.items()}

DEFAULT_GEN_ARGS = "--as-target --recurse --prune-root-groups ydb"
DEFAULT_TARGETS = ["all", "tests"]
DEFAULT_YA_DIRS = ["ydb/apps/ydbd", "ydb/apps/ydb"]

# what the hand-written BUILD.gn of contrib and of `keep` modules list
SOURCE_EXTS = (".c", ".cc", ".cpp", ".cxx", ".S", ".s", ".asm", ".rl6", ".rl", ".y", ".l",
               ".proto", ".fbs", ".gperf", ".ev", ".sfdl", ".pyx")
# the ya build configuration GN mirrors by hand
YA_CONFIG = ("build/ymake.core.conf", "build/ymake_conf.py", "build/conf/", "build/plugins/",
             "build/platform/", "build/external_resources/", "build/prebuilt/")
# ...but not the parts of other languages, GN builds C++ only
YA_CONFIG_OTHER = ("build/conf/ts/", "build/conf/java", "build/conf/go", "build/platform/java/",
                   "build/platform/go/", "build/platform/yfm/", "build/plugins/lib/nots/",
                   "build/plugins/nots.py", "build/plugins/java.py", "build/plugins/go.py",
                   "build/external_resources/pnpm/")
# changes worth a note in the journal (the bench builds with ya too)
INFO = (("ya", "ya"), ("ya.conf", "ya"), ("build/scripts/", "build scripts"))
# a commit touching only these builds nothing
NO_BUILD = (".github/", "ydb/docs/")


def read_text(path):
    """The text of a file (errors replaced, for logs and reports)."""
    with open(path, errors="replace") as f:
        return f.read()


def write_text(path, text):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


LINK_POOL_ARG = re.compile(r"^\s*link_pool_size\s*=")


def set_link_pool_size(path, jobs):
    """link_pool_size = jobs in args.gn, the rest of the file kept. The link
    steps get a pool of their own (//build/gn/config/CONFIG.gn), its depth
    follows ninja -j. True if the file changed."""
    text = read_text(path) if os.path.exists(path) else ""
    kept = [l for l in text.splitlines() if not LINK_POOL_ARG.match(l)]
    new = "\n".join(kept + ["link_pool_size = %d" % jobs]) + "\n"
    if new == text:
        return False
    write_text(path, new)
    return True


def gen_input(path):
    """A file of the build graph (`gn gen` reads it and lists it in its
    depfile): a BUILD.gn, a .gni, .gn, args.gn. A source reaches ninja, not
    gn gen. Only the *new* files are matched by this: a modified one is caught
    by its mtime against the depfile."""
    return (path in (".gn", "args.gn")
            or path.endswith("BUILD.gn") or path.endswith(".gni"))


def gen_inputs(out):
    """The paths the last `gn gen` read, from its depfile: the BUILD.gn, .gni,
    .gn, args.gn and the exec_script files. As ninja spells them, relative to
    the build dir; None if there is no depfile."""
    try:
        text = read_text(os.path.join(out, "build.ninja.d"))
    except OSError:
        return None
    tokens = text.replace("\\\n", " ").split()
    return tokens[1:] if tokens and tokens[0].endswith(":") else tokens


def listing(items, limit=40, indent="  "):
    """The items one per line (at most limit) for a STOP message."""
    items = list(items)
    shown = [indent + str(i) for i in items[:limit]]
    rest = len(items) - len(shown)
    return "\n".join(shown + ([indent + "... %d more" % rest] if rest > 0 else []))


class Stop(Exception):
    def __init__(self, kind, text):
        super().__init__(text)
        self.kind, self.text = kind, text


# ---- git ----------------------------------------------------------------------

class Git:
    def __init__(self, root):
        self.root = root

    def run(self, *args, check=True, env=None):
        r = subprocess.run(["git", "-C", self.root] + list(args), stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, universal_newlines=True, env=env)
        if check and r.returncode != 0:
            raise RuntimeError("git %s: %s" % (" ".join(args), r.stderr.strip()))
        return r

    def out(self, *args):
        return self.run(*args).stdout.strip()

    def lines(self, *args):
        return [l for l in self.out(*args).splitlines() if l]

    def merge_head(self):
        path = self.out("rev-parse", "--git-path", "MERGE_HEAD")
        path = os.path.join(self.root, path) if not os.path.isabs(path) else path
        if os.path.exists(path):
            with open(path) as f:
                return f.read().split()[0]
        return None

    def dirty(self):
        return self.lines("status", "--porcelain")

    def added_paths(self):
        """Paths the work tree has and HEAD does not: merged in or untracked.
        -z, split by NUL: git quotes a path with a space or a non-ASCII byte,
        and the caller matches on the extension."""
        out = []
        for args in (("diff", "--name-only", "--diff-filter=A", "-z", "HEAD"),
                     ("ls-files", "--others", "--exclude-standard", "-z")):
            out += [p for p in self.run(*args).stdout.split("\0") if p]
        return out

    def unmerged(self):
        return self.lines("diff", "--name-only", "--diff-filter=U")

    def subject(self, rev):
        return self.out("log", "-1", "--format=%s", rev)

    def short(self, rev):
        return self.out("rev-parse", "--short=11", rev)

    def follow_step(self):
        """(upstream SHA, sync SHA or None), or None for an unmanaged history."""
        row = self.out("log", "--first-parent", "--merges", "-1", "--format=%H %P%x09%s", "HEAD")
        merge = self.merge_step(row) if row else None
        if not merge:
            return None
        merge_commit, upstream = merge
        row = self.out("log", "--first-parent", "-1", "--format=%H %P%x09%s", "--extended-regexp",
                       "--grep=^gn: sync [0-9a-f]{7,40}:", merge_commit + "..HEAD")
        if not row:
            return upstream, None
        parents, _, subject = row.partition("\t")
        match = re.match(r"^gn: sync ([0-9a-f]{7,40}):", subject)
        parents = parents.split()
        if len(parents) != 2 or not upstream.startswith(match.group(1)):
            raise Stop("conflict", "follow sync message does not match the latest follow merge: %s" % row)
        return upstream, parents[0]

    def merge_step(self, row):
        """(merge SHA, upstream SHA) of a `Merge <U>:` first-parent merge, or None."""
        commits, _, subject = row.partition("\t")
        match = re.match(r"^Merge ([0-9a-f]{7,40}):", subject)
        if not match:
            return None
        parents = commits.split()
        if len(parents) != 3 or not parents[2].startswith(match.group(1)):
            raise Stop("conflict", "follow merge message does not match its second parent: %s" % row)
        return parents[0], parents[2]

# ---- journal ------------------------------------------------------------------

class State:
    def __init__(self, path):
        self.path = path
        os.makedirs(os.path.join(path, "steps"), exist_ok=True)
        self.journal = os.path.join(path, "journal.jsonl")
        self.report = os.path.join(path, "report.txt")
        self.blocked = os.path.join(path, "blocked.txt")
        self.stopped = os.path.join(path, "STOPPED.md")
        self._preview = []

    def records(self):
        if not os.path.exists(self.journal):
            return list(self._preview)
        with open(self.journal) as f:
            return [json.loads(l) for l in f if l.strip()] + self._preview

    def append(self, rec):
        rec = dict(rec, time=time.strftime("%Y-%m-%dT%H:%M:%S"))
        with open(self.journal, "a") as f:
            f.write(json.dumps(rec, sort_keys=True) + "\n")

    def done(self):
        """The upstream commits whose steps are done (the bootstrap's one too)."""
        return {r["upstream"] for r in self.records() if r["status"] in ("done", "bootstrap")}

    def bootstrapped(self):
        return any(r["status"] in ("bootstrap", "recovered") for r in self.records())

    def restore(self, git, upstream, persist=False):
        """Seed a missing journal from history; never import local build/bench results."""
        if self.bootstrapped() or any(r.get("from_history") for r in self.records()):
            return None
        history = git.follow_step()
        if not history:
            return None
        u, sync = history
        if git.run("merge-base", "--is-ancestor", u, upstream, check=False).returncode != 0 \
                and git.run("merge-base", "--is-ancestor", upstream, u, check=False).returncode != 0:
            raise Stop("conflict", "the last followed commit %s and upstream %s have diverged; "
                       "check --upstream before continuing" % (git.short(u), upstream))
        rec = {"upstream": u, "status": "done" if sync else "recovered", "from_history": True,
               "result": sync or git.out("rev-parse", "HEAD"), "subject": git.subject(u),
               "seconds": {}, "time": time.strftime("%Y-%m-%dT%H:%M:%S")}
        if persist:
            self.append(rec)
        else:
            self._preview.append(rec)
        return rec

    def last_merged(self, git, upstream):
        """Use the restored journal anchor if the upstream ref is older than it."""
        last = git.out("merge-base", "HEAD", upstream)
        for rec in self.records():
            if rec.get("from_history") or rec["status"] == "recovered":
                u = rec["upstream"]
                if git.run("merge-base", "--is-ancestor", last, u, check=False).returncode == 0 \
                        and git.run("merge-base", "--is-ancestor", u, "HEAD", check=False).returncode == 0:
                    return u
                break
        return last

    def last_stop(self):
        recs = [r for r in self.records() if r["status"] != "ack"]
        return recs[-1] if recs and recs[-1]["status"] == "stopped" else None

    def stopped_at(self, u):
        """The stages the step of u stopped at so far."""
        return {r["stage"] for r in self.records() if r["status"] == "stopped" and r["upstream"] == u}

    def step_dir(self, name):
        d = os.path.join(self.path, "steps", name)
        os.makedirs(d, exist_ok=True)
        return d


# ---- classification -----------------------------------------------------------

def changes(git, since, u):
    """[(status letter, path)] of U against `since` (U^1 for a single commit)."""
    out = []
    for line in git.lines("diff", "--name-status", "--no-renames", since, u):
        status, path = line.split("\t", 1)
        out.append((status[0], path))
    return out


def keep_dirs(git):
    return {os.path.dirname(p) for p in
            git.run("grep", "-l", "-e", "yamake2gn: keep", "--", "*BUILD.gn", check=False).stdout.split()}


def is_listed(status, path):
    """Does the change show in a hand-written BUILD.gn listing the sources?"""
    name = os.path.basename(path)
    return name == "ya.make" or name.endswith(".inc") or (status in "AD" and name.endswith(SOURCE_EXTS))


def keep_owner(root, keeps, path):
    """The `keep` module dir the path belongs to, or None."""
    x = os.path.dirname(path)
    while x:
        if x in keeps:
            return x
        if os.path.exists(os.path.join(root, x, "ya.make")):
            return None
        x = os.path.dirname(x)
    return None


def classify(root, git, u, keeps, since=None):
    """{"review": [(why, path)], "info": [(what, path)], "files": N, "build": bool}

    since: the base of the diff (U^1 unless the step spans many commits)."""
    review, info, build = [], [], False
    ch = changes(git, since or u + "^1", u)
    for status, path in ch:
        if not path.startswith(NO_BUILD) and not path.endswith(".md"):
            build = True
        if path.startswith("contrib/"):
            if is_listed(status, path):
                review.append(("contrib", "%s %s" % (status, path)))
        elif path.startswith(YA_CONFIG):
            if not path.startswith(YA_CONFIG_OTHER):
                review.append(("ya config", "%s %s" % (status, path)))
        elif is_listed(status, path):
            owner = keep_owner(root, keeps, path)
            if owner is not None:
                review.append(("keep //%s" % owner, "%s %s" % (status, path)))
        for prefix, what in INFO:
            if path == prefix or (prefix.endswith("/") and path.startswith(prefix)):
                info.append((what, "%s %s" % (status, path)))
    return {"review": review, "info": info, "files": len(ch), "build": build}


# ---- generator report -----------------------------------------------------------

NINJA_PROGRESS = re.compile(r"^\[\d+/\d+\] ")
ERROR_LINE = re.compile(r"\berror\b|undefined (reference|symbol)|Traceback|No such file", re.I)


def failed_commands(log_path):
    """The FAILED blocks of a ninja log: (line number, output, error lines, other lines)."""
    blocks, cur = [], None
    try:
        with open(log_path, errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return blocks
    for n, line in enumerate(lines, 1):
        if line.startswith("FAILED: "):
            cur = {"line": n, "output": line[8:].split()[0] if line[8:].split() else "?", "body": []}
            blocks.append(cur)
            cur["skip_command"] = True      # the line after FAILED is the command
        elif cur is not None:
            if NINJA_PROGRESS.match(line) or line.startswith("ninja: "):
                cur = None
            elif cur.pop("skip_command", False):
                pass
            elif line.strip():
                cur["body"].append(line)
    return [(b["line"], b["output"], [l for l in b["body"] if ERROR_LINE.search(l)], b["body"]) for b in blocks]


def describe_failures(log_path, limit=20, errors_per_command=3):
    """The failed commands of a ninja log with their first errors, for a STOP message."""
    out, blocks = [], failed_commands(log_path)
    name = os.path.basename(log_path)
    for line, output, errors, body in blocks[:limit]:
        out.append("  %s  (%s:%d)" % (output, name, line))
        shown = (errors or body)[:errors_per_command]
        out += ["      " + l.strip()[:300] for l in shown]
        rest = len(errors or body) - len(shown)
        if rest > 0:
            out.append("      ... %d more %s" % (rest, "error lines" if errors else "lines"))
    if len(blocks) > limit:
        out.append("  ... %d more failed commands" % (len(blocks) - limit))
    return out


def report_problems(text):
    """{heading: {item}} of the sections of a yamake2gn report that need a fix:
    `skipped (non-trivial)` and the ERROR ones."""
    start = text.rfind("==================== yamake2gn report")
    sections, cur = {}, None
    for line in text[max(start, 0):].splitlines()[1:]:
        if not line.strip():
            cur = None
        elif not line.startswith(" "):
            cur = line[:-1] if line.endswith(":") and line.startswith(("ERROR", "skipped")) else None
            if cur:
                sections.setdefault(cur, set())
        elif cur:
            sections[cur].add(" ".join(line.split()))
    return sections


def format_problems(sections):
    """A report of only the problem sections, parsed back the same by report_problems()."""
    out = ["==================== yamake2gn report: accepted problems"]
    for heading in sorted(sections):
        out += ["", heading + ":"] + ["  " + i for i in sorted(sections[heading])]
    return "\n".join(out) + "\n"


def strip_new_blocks(text):
    """The report file without the blocks of new problems not accepted yet; were there any."""
    out, inside, found = [], False, False
    for line in text.splitlines():
        if line.startswith(REPORT_NEW):
            inside = found = True
        elif inside and line.startswith(REPORT_END):
            inside = False
        elif not inside:
            out.append(line)
    return "\n".join(out) + "\n", found


def new_problems(old_text, new_text):
    old = report_problems(old_text)
    new = {}
    for heading, items in report_problems(new_text).items():
        added = items - old.get(heading, set())
        if added:
            new[heading] = sorted(added)
    return new


def has_new_problem_blocks(path):
    """Does the accepted report of the branch still hold the blocks of new problems?"""
    if not os.path.exists(path):
        return False
    return strip_new_blocks(read_text(path))[1]


# ---- progress -----------------------------------------------------------------

def fmt_seconds(s):
    s = int(s)
    if s < 60:
        return "%ds" % s
    if s < 3600:
        return "%dm%02ds" % (s // 60, s % 60)
    return "%dh%02dm" % (s // 3600, s % 3600 // 60)


class LogTail:
    """The complete lines a growing log got since the last call."""
    def __init__(self, path, from_end=False):
        self.path, self.rest = path, b""
        self.pos = os.path.getsize(path) if from_end and os.path.exists(path) else 0

    def lines(self):
        try:
            with open(self.path, "rb") as f:
                if os.fstat(f.fileno()).st_size < self.pos:      # rewritten
                    self.pos, self.rest = 0, b""
                f.seek(self.pos)
                data = f.read()
                self.pos = f.tell()
        except OSError:
            return []
        parts = (self.rest + data).split(b"\n")
        self.rest = parts.pop()
        return [p.decode("utf-8", "replace") for p in parts]


NINJA_STATUS_RE = re.compile(r"^\[(\d+)/(\d+)\] ")    # NINJA_STATUS="[%f/%t] " of autofix.py, follow_bench.py


class NinjaProbe:
    """Done/total edges, failed commands and the time left of a ninja log."""
    def __init__(self, path):
        self.tail, self.done, self.total, self.failed = LogTail(path), None, None, 0
        self.samples = collections.deque()

    def poll(self):
        for line in self.tail.lines():
            m = NINJA_STATUS_RE.match(line)
            if m:
                self.done, self.total = int(m.group(1)), int(m.group(2))
            elif line.startswith("FAILED: "):
                self.failed += 1
        if self.done is None:
            return ["starting"]
        now = time.time()
        self.samples.append((now, self.done))
        while now - self.samples[0][0] > 300:        # the rate of the last 5 minutes
            self.samples.popleft()
        out = ["%d/%d (%d%%)" % (self.done, self.total, 100 * self.done // max(self.total, 1))]
        if self.failed:
            out.append("failed %d" % self.failed)
        (t0, d0), (t1, d1) = self.samples[0], self.samples[-1]
        if t1 - t0 >= 60 and d1 > d0:
            out.append("~%s left" % fmt_seconds((self.total - self.done) * (t1 - t0) / (d1 - d0)))
        return out

    def close(self):
        pass


class AutofixProbe:
    """The ninja run of autofix.py going on now and the edges it uncommented."""
    def __init__(self, log_dir):
        self.dir, self.start = log_dir, time.time()
        self.fixes = LogTail(os.path.join(log_dir, "fixes.log"), from_end=True)   # appended across runs
        self.n, self.ninja, self.fixed = 0, None, 0

    def poll(self):
        runs = []
        for name in os.listdir(self.dir) if os.path.isdir(self.dir) else []:
            m = re.match(r"^run(\d+)\.log$", name)
            if m and os.path.getmtime(os.path.join(self.dir, name)) >= self.start - 1:
                runs.append(int(m.group(1)))
        if runs and max(runs) != self.n:
            self.n = max(runs)
            self.ninja = NinjaProbe(os.path.join(self.dir, "run%d.log" % self.n))
        for line in self.fixes.lines():
            if " uncommented " in line:
                self.fixed += 1
            elif " re-commented " in line:
                self.fixed -= 1
        if not self.ninja:
            return ["starting"]
        return ["run %d" % self.n] + self.ninja.poll() + (["fixed %d" % self.fixed] if self.fixed else [])

    def close(self):
        pass


class YaProbe:
    """Tasks done and running, from the event log ya make writes as it goes."""
    def __init__(self, cache_root):
        self.dir, self.start = os.path.join(cache_root, "evlogs"), time.time()
        self.proc, self.started, self.finished = None, 0, 0

    def find(self):
        found = []
        for d, _, files in os.walk(self.dir):
            for name in files:
                path = os.path.join(d, name)
                if name.endswith(".evlog.zst") and os.path.getmtime(path) >= self.start - 1:
                    found.append((os.path.getmtime(path), path))
        return max(found)[1] if found else None

    def read(self):
        for line in self.proc.stdout:
            if '"node-started"' in line:
                self.started += 1
            elif '"node-finished"' in line:
                self.finished += 1

    def poll(self):
        if self.proc is None:
            path = self.find()
            if not path:
                return ["starting"]
            self.proc = subprocess.Popen(["sh", "-c", 'tail -c +1 -f --pid="$1" -- "$0" | zstd -qdc', path,
                                          str(os.getpid())],     # --pid: no orphan if this run is killed
                                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                         universal_newlines=True, errors="replace", start_new_session=True)
            threading.Thread(target=self.read, daemon=True).start()
        if not self.started:
            return ["configuring"]
        return ["%d tasks done, %d running" % (self.finished, self.started - self.finished)]

    def close(self):
        if self.proc:
            with contextlib.suppress(OSError):
                os.killpg(self.proc.pid, signal.SIGTERM)
            self.proc.wait()


class BenchProbe:
    """follow_bench.py: forwards its lines (bench.log), probes the build going on."""
    def __init__(self, step, ya_cache_root, emit):
        self.step, self.ya_cache_root, self.emit = step, ya_cache_root, emit
        self.log = LogTail(os.path.join(step, "bench.log"))
        self.phase, self.probe, self.since = None, None, time.time()

    def switch(self, phase, probe=None):
        if self.probe:
            self.probe.close()
        self.phase, self.probe, self.since = phase, probe, time.time()

    def poll(self):
        for line in self.log.lines():
            self.emit(line)
            text = line.split("bench: ", 1)[-1]
            if text.startswith("ninja warm-up in "):
                self.switch("ninja warm-up", NinjaProbe(os.path.join(self.step, "bench_ninja_warm.log")))
            elif text.startswith("ya warm-up in "):
                self.switch("ya warm-up", YaProbe(self.ya_cache_root))
            elif re.match(r"^ninja [0-9a-f]+$", text):
                self.switch("ninja", NinjaProbe(os.path.join(self.step, "bench_ninja.log")))
            elif re.match(r"^ya [0-9a-f]+$", text):
                self.switch("ya", YaProbe(self.ya_cache_root))
            elif text.startswith("load "):
                self.switch("waiting for the machine to idle: " + text.split(",")[0])
            elif re.match(r"^(ninja warm-up|ya warm-up|ninja|ya): rc ", text):
                self.switch(None)
        if not self.phase:
            return []
        return ["%s (%s)" % (self.phase, fmt_seconds(time.time() - self.since))] \
            + (self.probe.poll() if self.probe else [])

    def close(self):
        self.switch(None)


class Progress:
    """On a terminal, one status line of the stage going on, rewritten in place
    (not in run.log); the probe of the stage runs either way (it may forward lines)."""
    def __init__(self, term):
        tty = term.isatty() and os.environ.get("TERM", "dumb") != "dumb"
        self.term, self.lock, self.shown = term if tty else None, threading.Lock(), False

    def print(self, text):
        with self.lock:
            self.clear()
            print(text, flush=True)

    def clear(self):
        if self.shown:
            self.term.write("\r\x1b[K")
            self.term.flush()
            self.shown = False

    @contextlib.contextmanager
    def stage(self, title, probe=None, last=None):
        if not self.term and not probe:
            yield
            return
        stop, t0 = threading.Event(), time.time()

        def tick():
            parts = probe.poll() if probe else []
            if self.term:
                parts = parts + [fmt_seconds(time.time() - t0)] + (["last %s" % fmt_seconds(last)] if last else [])
                width = shutil.get_terminal_size((120, 20)).columns
                width = width - 1 if width > 20 else 120     # 0: a terminal of unknown size
                with self.lock:
                    self.term.write("\r\x1b[K" + ("%s %s: %s" % (time.strftime("%H:%M:%S"), title,
                                                                    ", ".join(parts)))[:width])
                    self.term.flush()
                    self.shown = True

        def loop():
            while True:
                try:
                    tick()
                except Exception:       # the progress never breaks the run
                    pass
                if stop.wait(1):
                    return

        thread = threading.Thread(target=loop, daemon=True)
        thread.start()
        try:
            yield
        finally:
            stop.set()
            thread.join()
            if probe:
                with contextlib.suppress(Exception):
                    probe.poll()        # the last lines
                probe.close()
            with self.lock:
                self.clear()


# ---- the step -----------------------------------------------------------------

def fetch_upstream(git, upstream, enabled):
    """git fetch of a remote ref, e.g. origin/main."""
    if enabled and "/" in upstream:
        remote, _, branch = upstream.partition("/")
        git.out("fetch", "-q", remote, branch)


def pending_commits(git, state, upstream):
    """(last followed commit, merge in progress, first-parent commits after it)."""
    last = state.last_merged(git, upstream)
    pending = git.lines("rev-list", "--first-parent", "--reverse", last + ".." + upstream)
    return last, git.merge_head(), pending


class Follower:
    def __init__(self, args, term):
        self.args = args
        self.progress = Progress(term)
        self.root = os.path.realpath(args.root)
        self.out = os.path.realpath(args.out) if args.out else None
        self.git = Git(self.root)
        self.state = State(os.path.realpath(args.state))
        self.env = dict(os.environ)
        for kv in args.env:
            name, _, value = kv.partition("=")
            self.env[name] = value

    def say(self, text):
        self.progress.print("%s %s" % (time.strftime("%H:%M:%S"), text))

    def last_seconds(self, stage):
        """How long the stage took last time (a bootstrap aside), for the progress line."""
        for r in reversed(self.state.records()):
            s = (r.get("seconds") or {}).get(stage)
            if s and r.get("status") != "bootstrap":
                return s
        return None

    def sh(self, cmd, log, cwd=None):
        """Runs cmd with its output into the log; (rc, seconds)."""
        t = time.time()
        with open(log, "w") as f:
            rc = subprocess.run(cmd, cwd=cwd or self.root, stdout=f, stderr=subprocess.STDOUT,
                                env=self.env).returncode
        return rc, round(time.time() - t, 1)

    # -- stages

    def merge(self, u, rec):
        t = time.time()
        r = self.git.run("-c", "rerere.enabled=true", "-c", "rerere.autoUpdate=true", "merge", "--no-ff",
                         "--no-edit", "-m", "Merge %s: %s" % (self.git.short(u), self.git.subject(u)), u,
                         check=False)
        if r.returncode != 0:
            if not self.git.merge_head():
                raise Stop("conflict", "git merge %s failed:\n%s" % (u, (r.stdout + r.stderr).strip()))
            self.finish_merge(u)
        rec["seconds"]["merge"] = round(time.time() - t, 1)

    def finish_merge(self, u):
        left = self.git.unmerged()
        if left:
            raise Stop("conflict", "merge of %s left conflicts (rerere did not resolve them):\n%s\n\n"
                       "Resolve them and commit the merge (or leave it resolved and uncommitted), then run again."
                       % (u, "\n".join("  " + p for p in left)))
        self.git.out("commit", "--no-edit")

    def review_markers(self):
        """The files with review markers left (an emptied REVIEW_FILE is removed)."""
        r = self.git.run("grep", "--no-recurse-submodules", "-l", "--untracked", "-E", "-e",
                         REVIEW_MARK.pattern, check=False)
        if r.returncode not in (0, 1):
            raise RuntimeError("git grep: %s" % r.stderr.strip())
        left = [l for l in r.stdout.splitlines() if l]
        review_file = os.path.join(self.root, REVIEW_FILE)
        if REVIEW_FILE not in left and os.path.exists(review_file):
            os.remove(review_file)
        return left

    def review(self, u, cls):
        """Puts a marker into every BUILD.gn to mirror U into by hand; the step goes on once they are gone."""
        if not cls["review"]:
            return
        if "review" in self.state.stopped_at(u):
            left = self.review_markers()
            if left:
                raise Stop("review", "review markers left (`gn-follow: REVIEW` lines) in:\n%s\n\n"
                           "Mirror the change by hand and delete the marker lines, then run again."
                           % listing(left))
            return
        short, targets = self.git.short(u), {}
        for why, item in cls["review"]:
            path = item.split(" ", 1)[1]
            target = REVIEW_FILE
            if why.startswith("keep //"):
                target = why[len("keep //"):] + "/BUILD.gn"
            elif why == "contrib":
                d = os.path.dirname(path)
                while d and not os.path.exists(os.path.join(self.root, d, "BUILD.gn")):
                    d = os.path.dirname(d)
                target = d + "/BUILD.gn" if d else REVIEW_FILE
            targets.setdefault(target, []).append("%s (%s)" % (item, why))
        for target, items in sorted(targets.items()):
            full = os.path.join(self.root, target)
            old = read_text(full) if os.path.exists(full) else ""
            prefix = "" if target == REVIEW_FILE else "# "
            write_text(full, "".join("%sgn-follow: REVIEW %s: %s\n" % (prefix, short, i) for i in items) + old)
        raise Stop("review", "%s touches what the generator does not carry over; a marker line "
                   "(`gn-follow: REVIEW`) is put at the top of:\n%s\n\nMirror the change there by hand "
                   "(nothing to mirror: just delete the line), delete the marker lines, then run again."
                   % (short, listing("%s\n%s" % (t, listing(items, 20, "      "))
                                     for t, items in sorted(targets.items())), 30))

    def regen(self, step, rec, idempotence):
        py = self.args.python
        cmd = [py, os.path.join(self.root, "build/gn/scripts/yamake2gn.py"), "--root", self.root,
               "--build-dir", self.out] + shlex.split(self.args.gen_args)
        before = set(self.git.dirty())
        t = time.time()
        with open(os.path.join(step, "yamake2gn.out"), "w") as o, \
                open(os.path.join(step, "yamake2gn.txt"), "w") as e, \
                self.progress.stage("  regen: yamake2gn.py", last=self.last_seconds("regen")):
            rc = subprocess.run(cmd, cwd=self.root, stdout=o, stderr=e, env=self.env).returncode
        rec["seconds"]["regen"] = round(time.time() - t, 1)
        report = read_text(os.path.join(step, "yamake2gn.txt"))
        m = re.search(r"generated : (\d+)\s+deleted\(BUILD.gn\): (\d+)", report)
        if m:
            rec["generated"], rec["deleted"] = int(m.group(1)), int(m.group(2))
        if rc != 0:
            raise Stop("generator", "yamake2gn.py failed (rc %d), see %s/yamake2gn.txt" % (rc, step))
        changed = sorted(set(self.git.dirty()) - before)
        rec["regen_changed"] = len(changed)
        self.say("  regen: %d files changed, %s" % (len(changed), fmt_seconds(rec["seconds"]["regen"])))
        if idempotence and changed:
            raise Stop("idempotence", "the generator changed the starting tree (%d files):\n%s\n\n"
                       "Commit it (any message) to take it as the starting point, or fix the "
                       "generator; then run again." % (len(changed), listing(changed, 50)))
        self.check_report(step, rec, report)

    def check_report(self, step, rec, report):
        """Compares the generator report with the accepted one; STOP on the new problems."""
        path = os.path.join(self.root, REPORT_FILE)
        text = read_text(path) if os.path.exists(path) else None
        if text is None and os.path.exists(self.state.report):      # before the report moved into the branch
            text = format_problems(report_problems(read_text(self.state.report)))
        accepted, _ = strip_new_blocks(text) if text is not None else ("", False)
        new = new_problems(accepted, report) if text is not None else {}
        if new:
            short = self.git.short(rec["upstream"]) if len(rec["upstream"]) == 40 else rec["upstream"]
            write_text(path, accepted.rstrip("\n") + "\n\n" + "".join(
                "%s%s: delete this line and the %s one to accept\n%s:\n%s%s\n" % (
                    REPORT_NEW, short, REPORT_END, h, "".join("  %s\n" % i for i in items), REPORT_END)
                for h, items in new.items()))
            raise Stop("generator", "new problems in the generator report (%s/yamake2gn.txt), written "
                       "into %s between %s... and %s lines:\n%s\n\nFix them (ya.make, a keep BUILD.gn, ...) "
                       "or accept them by deleting those marker lines; then run again."
                       % (step, REPORT_FILE, REPORT_NEW, REPORT_END,
                          "\n".join("%s:\n%s" % (h, "\n".join("  " + i for i in items))
                                    for h, items in new.items())))
        write_text(path, format_problems(report_problems(report)))

    def need_gen(self, rec):
        """Whether `gn gen` has anything new to read. The generator writes only
        what changed (yamake2gn keeps the mtime of an identical file), so an
        input of the previous gen older than its build.ninja means the build
        files are still valid; regenerating would also restart the pch checks,
        `compile_commands.json` is in their depfile. A new BUILD.gn/.gni is not
        in the depfile yet: it forces a run too. Ninja has the same edge
        (`rule gn --regeneration` in build.ninja), so a miss costs the gen, not
        a stale build."""
        build_ninja = os.path.join(self.out, "build.ninja")
        if not os.path.exists(build_ninja):
            return True
        if rec.get("args_gn_changed"):
            return True
        inputs = gen_inputs(self.out)
        if inputs is None:
            return True
        done = os.stat(build_ninja).st_mtime_ns
        for p in inputs:
            try:
                st = os.stat(os.path.join(self.out, p))
            except OSError:
                return True             # the last gen read it, now it is gone
            if st.st_mtime_ns > done:
                return True
        return any(gen_input(p) for p in self.git.added_paths())

    def gn_gen(self, step, rec):
        if self.args.jobs and set_link_pool_size(os.path.join(self.out, "args.gn"), self.args.jobs):
            rec["args_gn_changed"] = True
            self.say("  args.gn: link_pool_size = %d" % self.args.jobs)
        if not self.need_gen(rec):
            rec["gen_skipped"] = True
            self.say("  gn gen: skipped, no input changed")
            return
        with self.progress.stage("  gn gen", last=self.last_seconds("gn_gen")):
            rc, s = self.sh(["gn", "gen", "--script-executable=" + self.args.script_executable, self.out],
                            os.path.join(step, "gn_gen.log"))
        rec["seconds"]["gn_gen"] = s
        if rc != 0:
            with open(os.path.join(step, "gn_gen.log")) as f:
                text = f.read()
            raise Stop("gn-gen", "gn gen failed:\n%s" % text[-4000:])
        self.say("  gn gen: %s" % fmt_seconds(s))

    def build(self, step, rec):
        summary = os.path.join(step, "autofix.json")
        cmd = [self.args.python, os.path.join(SCRIPTS, "autofix.py"),
               "--root", self.root, "--out", self.out, "--log-dir", os.path.join(step, "autofix"),
               "--blocked", self.state.blocked, "--summary", summary] \
            + (["-j", str(self.args.jobs)] if self.args.jobs else []) + self.args.targets
        self.say("  build: autofix.py %s" % " ".join(self.args.targets))
        with self.progress.stage("  build", AutofixProbe(os.path.join(step, "autofix")),
                                 last=self.last_seconds("build")):
            rc, s = self.sh(cmd, os.path.join(step, "autofix.log"))
        rec["seconds"]["build"] = s
        result = {}
        if os.path.exists(summary):
            with open(summary) as f:
                result = json.load(f)
        rec["build"] = {k: result.get(k) for k in ("rc", "runs", "failed")}
        rec["build"]["fixed"] = len(result.get("fixed", []))
        self.say("  build: rc %s after %s run(s), %s failed, %d edges uncommented, %s" % (
            rc, result.get("runs"), result.get("failed"), rec["build"]["fixed"], fmt_seconds(s)))
        if rc != 0:
            log = os.path.join(step, "autofix", "run%s.log" % result.get("runs"))
            text = ["ninja: %s failed commands after %s run(s) of autofix.py (%d edges uncommented "
                    "by it)." % (result.get("failed"), result.get("runs"), len(result.get("fixed", [])))]
            failures = describe_failures(log)
            if failures:
                text += ["", "Failed commands and their first errors:"] + failures
            unresolved = result.get("unresolved", [])
            if unresolved:
                text += ["", "Missing deps that autofix.py recognized but could not fix:"] \
                    + ["  " + u for u in unresolved[:60]]
            elif failures:
                text += ["", "autofix.py recognized none of them as a missing dep: fix them by hand."]
            text += ["", "Full log: %s" % log,
                     "Fix the work tree (uncommitted) and run again."]
            raise Stop("build", "\n".join(text))

    def commit(self, message, rec, allow_empty=False):
        left = self.review_markers()
        if left:
            raise Stop("review", "review markers left (`gn-follow: REVIEW` lines) in:\n%s\n\nDelete them "
                       "once the change is mirrored, then run again." % listing(left))
        if has_new_problem_blocks(os.path.join(self.root, REPORT_FILE)):
            raise Stop("generator", "%s still has blocks of new problems: fix or accept them, then run "
                       "again." % REPORT_FILE)
        self.git.out("add", "-A")
        if allow_empty or self.git.run("diff", "--cached", "--quiet", check=False).returncode != 0:
            rec["sync_files"] = len(self.git.lines("diff", "--cached", "--name-only"))
            self.git.out("commit", "-q", "--allow-empty", "-m", message)
        rec["result"] = self.git.out("rev-parse", "HEAD")

    def bench(self, step, rec, full):
        """Measures HEAD in the bench (follow_bench.py); the trailers for its message."""
        if not self.args.bench:
            return []
        if not full and any(l.startswith(SKIP_TRAILER + ":") for l in
                            self.git.out("log", "-1", "--format=%B", "HEAD").splitlines()):
            rec["bench"] = "skipped"
            self.say("  bench: skipped (%s in the commit)" % SKIP_TRAILER)
            return []
        if self.git.dirty():
            raise Stop("bench", "the work tree has changes, the commit to measure is HEAD:\n%s"
                       % "\n".join("  " + l for l in self.git.dirty()[:30]))
        benched = sum(1 for r in self.state.records() if isinstance(r.get("bench"), dict))
        result = os.path.join(step, "bench.json")
        a = self.args
        cmd = [a.python, os.path.join(SCRIPTS, "follow_bench.py"), "--root", a.bench_root,
               "--out", a.bench_out, "--ya-out", a.ya_out, "--commit", self.git.out("rev-parse", "HEAD"),
               "--result", result, "--order", "ninja,ya" if benched % 2 == 0 else "ya,ninja",
               "--python", a.python, "--script-executable", a.script_executable,
               "--targets"] + (a.bench_targets or a.targets)
        cmd += ["--env=" + kv for kv in a.bench_env]
        cmd += (["-j", str(a.jobs)] if a.jobs else [])
        cmd += (["--ya-cache-size", a.ya_cache_size] if a.ya_cache_size else [])
        dirs = list(a.ya_dirs)
        if not full and not a.no_select:
            sel = follow_select.select(self.root, rec["upstream"])
            rec["select"] = sel
            dirs += [d for d in sel["tests"] + sel["modules"] if d not in dirs]
            self.say("  select: %s" % (" ".join(sel["tests"] + sel["modules"]) or "-"))
        # the commit before U: the bench tree is behind it if steps went without --bench
        merge = self.git.out("rev-list", "--first-parent", "--merges", "-n1", "HEAD")
        cmd += ["--ya-dirs"] + dirs + ([] if full else ["--warm", "--from", merge + "^1"])
        cmd += (["--ya-cache-root", a.ya_cache_root] if a.ya_cache_root else []) + (["--full"] if full else [])
        self.say("  bench%s" % (" (full)" if full else ""))
        probe = BenchProbe(step, a.ya_cache_root or os.environ.get("YA_CACHE_DIR") or
                           os.path.expanduser("~/.ya"), self.progress.print)
        with self.progress.stage("  bench", probe, last=None if full else self.last_seconds("bench")):
            rc, s = self.sh(cmd, os.path.join(step, "bench.log"))
        rec["seconds"]["bench"] = s
        self.say("  bench: rc %d, %s" % (rc, fmt_seconds(s)))
        data = {}
        if os.path.exists(result):
            with open(result) as f:
                data = json.load(f)
        rec["bench"] = {side: {k: v for k, v in data[side].items() if k not in ("executed_seconds", "evlog")}
                        for side in ("ninja_warm", "ninja", "ya") if data.get(side)}
        if data.get("ya_dirs"):
            rec["bench"]["ya_targets"] = {k: data.get(k) for k in ("ya_dirs", "ya_missing", "ya_warm")}
        if rc != 0 and (rec["bench"].get("ninja_warm") or {}).get("failed"):
            raise Stop("bench", "the warm-up of the bench failed: ninja does not build the previous commit "
                       "in %s (rc %s, %s)\nLog: %s/bench_ninja_warm*.log. The bench build dir is broken, not "
                       "the commit: look into it, then run again (the bench tree is not moved yet)."
                       % (self.args.bench_out, rec["bench"]["ninja_warm"].get("rc"),
                          rec["bench"]["ninja_warm"].get("failed"), step))
        if rc != 0:
            raise Stop("bench", "the bench build of %s failed (rc %d): %s\nLogs: %s/bench*.log. A finding of "
                       "the incremental build: look into it, do not fix it in the bench tree; --run --bench "
                       "measures again, or --run --skip-bench to go on without the measurement."
                       % (self.git.short("HEAD"), rc, ", ".join(
                           "%s: %s" % (k, v.get("failed")) for k, v in rec["bench"].items() if v.get("failed"))
                          or "see bench.log", step))
        return data["trailers"]

    def amend(self, trailers, rec):
        if trailers:
            msg = self.git.out("log", "-1", "--format=%B", "HEAD")
            self.git.out("commit", "-q", "--amend", "-m", msg.rstrip() + "\n\n" + "\n".join(trailers))
        rec["result"] = self.git.out("rev-parse", "HEAD")

    # -- steps

    def bootstrap(self):
        base = self.state.last_merged(self.git, self.args.upstream)
        name = "bootstrap-" + self.git.short(base)
        step = self.state.step_dir(name)
        rec = {"upstream": base, "status": "bootstrap", "seconds": {}, "env": self.args.env}
        self.say("bootstrap at %s (upstream %s)" % (self.git.short("HEAD"), self.git.short(base)))
        stopped = self.state.stopped_at(name)
        # an earlier run stopped past the check: what is in the work tree now is fixes
        check = not (stopped - {"idempotence"})

        def run():
            if "bench" not in stopped:
                if check and self.git.dirty():
                    raise Stop("idempotence", "the work tree must be clean to check the generator:\n%s"
                               % "\n".join("  " + l for l in self.git.dirty()[:30]))
                self.regen(step, rec, idempotence=check)
                self.gn_gen(step, rec)
                self.build(step, rec)
                self.commit("gn: regen at %s" % self.git.short(base), rec)
            self.bench(step, rec, full=True)     # no trailers: a build from scratch
            rec["result"] = self.git.out("rev-parse", "HEAD")

        self.guarded(name, rec, run)
        self.state.append(rec)

    def step(self, u, merged):
        short = self.git.short(u)
        step = self.state.step_dir(short)
        rec = {"upstream": u, "subject": self.git.subject(u), "status": "done", "seconds": {},
               "committed": self.git.out("log", "-1", "--format=%cI", u), "env": self.args.env}
        self.say("%s %s" % (short, rec["subject"][:90]))
        since = self.step_since(step, u)
        committed = "bench" in self.state.stopped_at(u)    # it stopped after the commit
        if self.args.only_head and not committed:
            rec["mode"] = "only-head"
            rec["skipped"] = len(self.git.lines("rev-list", "--first-parent", since + ".." + u))

        def run():
            if not committed:
                if not merged:
                    self.merge(u, rec)
                cls = classify(self.root, self.git, u, keep_dirs(self.git), since)
                rec.update(files=cls["files"], review=cls["review"], info=cls["info"])
                with open(os.path.join(step, "classify.json"), "w") as f:
                    json.dump(cls, f, indent=1)
                self.review(u, cls)
                # Recovery must establish a local baseline even for a docs-only
                # merge; a remote machine's successful build is not ours.
                if not cls["build"] and not self.git.dirty() and self.state.done():
                    rec["noop"] = True
                else:
                    self.regen(step, rec, idempotence=False)
                    self.gn_gen(step, rec)
                    self.build(step, rec)
                self.commit("gn: sync %s: %s" % (short, rec["subject"]), rec, allow_empty=True)
            self.amend(self.bench(step, rec, full=False), rec)

        self.guarded(u, rec, run)
        self.state.append(rec)
        if os.path.exists(self.state.stopped):
            os.remove(self.state.stopped)
        if self.args.push:
            self.git.out("push", "-q", self.args.push, "HEAD:refs/heads/" + self.git.out(
                "rev-parse", "--abbrev-ref", "HEAD"))

    def step_since(self, step, u):
        """The base of the step's diff: U^1, or the last followed commit with --only-head
        (the merge commits U, so the base cannot be recomputed after it: it is kept in
        the step dir)."""
        path = os.path.join(step, "meta.json")
        if os.path.exists(path):
            since = json.loads(read_text(path)).get("since")
            if since:
                return since
        since = u + "^1"
        if self.args.only_head:
            last = self.state.last_merged(self.git, self.args.upstream)
            if last != u:
                since = last
        write_text(path, json.dumps({"upstream": u, "since": since}) + "\n")
        return since

    def guarded(self, u, rec, body):
        try:
            body()
        except Stop as s:
            rec.pop("report", None)
            self.record_stop(u, rec, s)
            raise

    def record_stop(self, u, rec, s):
        self.state.append(dict(rec, status="stopped", stage=s.kind, upstream=u))
        with open(self.state.stopped, "w") as f:
            f.write("# STOP %s at %s\n\n%s\n" % (s.kind, u, s.text))

    # -- the loop

    def skip_bench(self):
        """--skip-bench: only when the last run stopped at the bench of a commit; marks the commit."""
        stop = self.state.last_stop()
        head = self.git.subject("HEAD")
        if not stop or stop["stage"] != "bench" or not re.match(r"^[0-9a-f]{40}$", stop["upstream"]) \
                or not head.startswith(tuple(p % self.git.short(stop["upstream"]) for p in ("gn: sync %s", "Merge %s"))):
            raise SystemExit("--skip-bench does not apply now: the last run did not stop at the bench of a "
                             "commit (see the status); drop the flag")
        self.git.out("commit", "-q", "--amend", "--allow-empty", "--no-edit", "--trailer",
                     "%s: the bench failed, skipped by hand %s" % (SKIP_TRAILER, time.strftime("%Y-%m-%d")))
        self.say("%s: %s added to the commit" % (self.git.short(stop["upstream"]), SKIP_TRAILER))

    def current(self):
        """(U, merged) of the step to do, or None when the upstream is reached."""
        last, merge, pending = pending_commits(self.git, self.state, self.args.upstream)
        if merge:
            self.finish_merge(merge)
            return merge, True
        if last not in self.state.done():
            return last, True
        if self.git.dirty():
            raise Stop("conflict", "the work tree has changes, but the step of %s is done:\n%s"
                       % (last, listing(self.git.dirty(), 30)))
        if self.args.only_head:
            pending = pending[-1:]
        return (pending[0], False) if pending else None

    def follow(self):
        restored = self.state.restore(self.git, self.args.upstream, persist=True)
        if restored:
            self.say("journal: restored %s step %s from history" %
                     (restored["status"], self.git.short(restored["upstream"])))
        if self.args.skip_bench:
            self.skip_bench()
        if not self.state.bootstrapped():
            self.bootstrap()
        steps = 0
        while self.args.max_commits is None or steps < self.args.max_commits:
            try:
                cur = self.current()
            except Stop as s:
                self.record_stop(self.git.merge_head() or "HEAD", {"seconds": {}}, s)
                raise
            if cur is None:
                self.say("the upstream is reached")
                break
            self.step(*cur)
            steps += 1
        return 0


def bench_summary(b):
    if isinstance(b, str):      # "skipped"
        return "bench " + b
    n, y = (b or {}).get("ninja") or {}, (b or {}).get("ya") or {}
    parts = []
    if n:
        parts.append("ninja %ss/%s actions" % (n.get("wall"), n.get("actions")))
    if y:
        parts.append("ya %ss/%s run" % (y.get("wall"), y.get("executed")))
    return ", ".join(parts) or "no bench"


def status(args):
    """What the follow-up stands at; changes nothing."""
    root = os.path.realpath(args.root)
    git, state = Git(root), State(args.state)
    restored = state.restore(git, args.upstream)
    say = print
    lock_path = os.path.join(args.state, "lock")
    running = False
    if os.path.exists(lock_path):
        with open(lock_path) as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                running = True
    say("Run:        %s" % ("IN PROGRESS (another follow.py holds the lock)" if running else "none"))
    say("State:      %s (run.log, journal.jsonl, steps/)" % args.state)
    cfg = {k: getattr(args, k) for k in CONFIG_DEFAULTS if getattr(args, k) != CONFIG_DEFAULTS[k]}
    lines = ["--%s %s" % (k.replace("_", "-"), " ".join(v) if isinstance(v, list) else v)
             for k, v in sorted(cfg.items())] or ["defaults"]
    say("Config:     " + "\n            ".join(lines))

    def changed(g):     # porcelain lines keep their leading space: Git.lines() would strip it
        return [l[3:] for l in g.run("status", "--porcelain").stdout.splitlines() if l]

    branch = git.out("rev-parse", "--abbrev-ref", "HEAD")
    say("Work tree:  %s  %s @ %s  %s" % (root, branch, git.short("HEAD"), git.subject("HEAD")[:70]))
    dirty, mh = changed(git), git.merge_head()
    if mh:
        say("            merge of %s in progress, %d unmerged files" % (git.short(mh), len(git.unmerged())))
    if dirty:
        say("            %d uncommitted changes: %s%s" % (len(dirty), ", ".join(dirty[:4]),
                                                        " ..." if len(dirty) > 4 else ""))

    fetch_head = git.out("rev-parse", "--git-path", "FETCH_HEAD")
    fetch_head = fetch_head if os.path.isabs(fetch_head) else os.path.join(root, fetch_head)
    fetched = time.strftime("%Y-%m-%d %H:%M", time.localtime(os.path.getmtime(fetch_head))) \
        if os.path.exists(fetch_head) else "never"
    last, mh, pending = pending_commits(git, state, args.upstream)
    say("Upstream:   %s @ %s (fetched %s; --fetch to update)" % (args.upstream, git.short(args.upstream), fetched))
    say("            last merged: %s %s" % (git.short(last), git.subject(last)[:70]))

    recs = state.records()
    if restored:
        say("Journal:    preview: %s step %s restored from history (not written)" %
            (restored["status"], git.short(restored["upstream"])))
    if not state.bootstrapped():
        say("Journal:    no bootstrap yet: the first run checks and builds the starting point")
    stop = state.last_stop()
    if stop:
        u = stop["upstream"]
        name = git.short(u) if re.match(r"^[0-9a-f]{40}$", u) else u
        say("Stopped:    %s at %s (%s)" % (stop["stage"], name, stop.get("time", "?").replace("T", " ")))
        say("            what to do: %s" % HINTS.get(stop["stage"], "see STOPPED.md"))
        say("            details: %s, logs: %s" % (state.stopped, os.path.join(state.path, "steps", name)))
    done = [r for r in recs if r["status"] in ("done", "bootstrap")]
    if done:
        say("Done:       %d commits; the last journal records:"
            % sum(1 for r in done if r["status"] == "done"))
        for r in done[-5:]:
            u = r["upstream"]
            kind = ("bootstrap: " if r["status"] == "bootstrap" else
                    "head: " if r.get("mode") == "only-head" else "docs: " if r.get("noop") else "")
            say("  %s %s  %-44s %s" % (git.short(u), r.get("time", "")[:16].replace("T", " "),
                                       kind + bench_summary(r.get("bench")), (r.get("subject") or "")[:60]))

    current = mh or (last if last not in state.done() else None)
    queue = ([current] if current else []) + [u for u in pending if u != current]
    ahead = len(queue)
    if args.only_head and queue:
        queue = queue[-1:]
    if args.only_head and ahead:
        say("Pending:    %d commits; --only-head: one step to the head of %s:"
            % (ahead, args.upstream))
    else:
        say("Pending:    %d commits%s" % (ahead, "" if not ahead else "; the next ones:"))
    keeps = keep_dirs(git)
    limit = None if args.max_commits in (None, "all") else args.max_commits
    show = queue if args.show == "all" else queue[:max(int(args.show), limit or 0)]
    since = last if args.only_head else None
    for u in show:
        c = classify(root, git, u, keeps, since)
        yamake = sum(1 for _, p in changes(git, since or u + "^1", u) if os.path.basename(p) == "ya.make")
        mark = "REVIEW" if c["review"] else ("docs" if not c["build"] else "")
        tail = [t for t in (mark,
                            "<- current" if u == current else "",
                            "[--run]" if limit is not None and queue.index(u) < limit else "",
                            "[--only-head]" if args.only_head else "") if t]
        say(("  %s %s %4d files %3d ya.make  %-70s%s" % (
            git.short(u), git.out("log", "-1", "--format=%cs", u), c["files"], yamake,
            git.subject(u)[:70], "".join("  " + t for t in tail))).rstrip())
        for why, path in (c["review"] + c["info"])[:40]:
            say("        %-24s %s" % (why, path))
        if len(c["review"] + c["info"]) > 40:
            say("        ... %d more" % (len(c["review"] + c["info"]) - 40))
    if len(show) < len(queue):
        say("  ... %d more (--show N or --show all)" % (len(queue) - len(show)))

    if args.bench_root:
        bgit = Git(args.bench_root)
        bdirty = changed(bgit)
        say("Bench:      %s @ %s  %s" % (args.bench_root, bgit.short("HEAD"), bgit.subject("HEAD")[:60]))
        say("            %s" % ("clean" if not bdirty else "CHANGES (the bench refuses to run): " +
                                ", ".join(bdirty[:4])))
        say("            measured only with --run --bench")
        if args.bench_out and not os.path.exists(os.path.join(args.bench_out, ".ninja_log")):
            say("            no .ninja_log in %s: the ninja warm-up will rebuild everything" % args.bench_out)
    say("")
    branch = args.upstream.partition("/")[2]
    say("To go on: build/gn/scripts/follow.py --run%s [--max-commits N]%s"
        % (" --branch " + branch if branch and branch != "main" else "",
           " [--skip-bench]" if stop and stop["stage"] == "bench" else ""))
    return 0


# what a run remembers in <state>/config.json: an argument given overrides it and is saved
CONFIG_DEFAULTS = {
    "out": None,                # <root>/out/gn
    "env": [],
    "jobs": None,
    "nice": 10,                 # niceness of the run and everything it starts (0: off)
    "targets": DEFAULT_TARGETS,
    "gen_args": DEFAULT_GEN_ARGS,
    "push": None,
    "bench_root": None,
    "bench_out": None,          # <bench root>/out/gn
    "ya_out": None,
    "ya_cache_root": None,
    "ya_cache_size": None,
    "ya_dirs": DEFAULT_YA_DIRS,
    "no_select": False,
    "bench_env": [],
    "bench_targets": None,
}
CONFIG_PATHS = ("out", "bench_root", "bench_out", "ya_out", "ya_cache_root")


def state_name(upstream):
    """The state subdir of an upstream ref: origin/main -> main, origin/ydb/stable -> ydb-stable."""
    ref = upstream.partition("/")[2] or upstream
    return re.sub(r"[^A-Za-z0-9._-]+", "-", ref).strip("-") or "default"


def load_config(args, path):
    """Fills the arguments not given from the config; (the whole config, the values given)."""
    cfg = {}
    if os.path.exists(path):
        with open(path) as f:
            cfg = json.load(f)
    given = {}
    for key, default in CONFIG_DEFAULTS.items():
        value = getattr(args, key)
        if value is None:
            setattr(args, key, cfg.get(key, default))
            continue
        if key in CONFIG_PATHS:
            value = os.path.realpath(value)
        setattr(args, key, value)
        if cfg.get(key) != value:
            given[key] = value
        cfg[key] = value
    return cfg, given


def save_config(path, cfg, given):
    """Saves the arguments given (they override the remembered ones from now on)."""
    if not given:
        return
    write_text(path, json.dumps(cfg, indent=1, sort_keys=True))
    print("config: saved %s" % ", ".join("--" + k.replace("_", "-") for k in sorted(given)))


def resolve_upstream(ap, args):
    """--branch NAME follows origin/NAME; --upstream REF overrides it."""
    if args.branch and args.upstream:
        ap.error("give --branch or --upstream, not both")
    return args.upstream or "origin/" + (args.branch or "main")


def validate(ap, args):
    """The checks of the arguments as they are with the remembered settings."""
    if args.max_commits == "all":
        args.max_commits = None
    if args.only_head and args.max_commits is not None:
        ap.error("--only-head goes to the head in one step: drop --max-commits")
    if not args.run and (args.skip_bench or args.bench):
        ap.error("--skip-bench and --bench go with --run")
    if args.bench and not args.bench_root:
        ap.error("--bench needs --bench-root")
    if args.bench and not args.ya_out:
        ap.error("--bench-root needs --ya-out")
    if args.nice and args.nice < 0:
        ap.error("--nice must be 0 or more")


def require_ref(ap, root, ref):
    """A clear error before the status or the run uses the ref."""
    if subprocess.run(["git", "-C", root, "rev-parse", "--verify", "--quiet", ref + "^{commit}"],
                      stdout=subprocess.PIPE, stderr=subprocess.PIPE).returncode != 0:
        ap.error("%s is not fetched: run with --fetch, or check --branch" % ref)


class Tee:
    """Writes to a stream and to the run log of the state dir."""
    def __init__(self, stream, log):
        self.stream, self.log = stream, log

    def write(self, text):
        self.stream.write(text)
        self.log.write(text)
        self.log.flush()

    def flush(self):
        self.stream.flush()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", help="the work tree of the GN branch (default: the one of the cwd)")
    ap.add_argument("--state", help="journal, logs, config (default: <git dir of the work "
                                    "tree>/gn-follow/<branch>)")
    ap.add_argument("--branch", metavar="NAME",
                    help="the upstream branch to follow: origin/NAME (default: main; every branch "
                         "has its own state)")
    ap.add_argument("--upstream", metavar="REF", help="the ref to follow instead of origin/<branch>")
    ap.add_argument("--run", action="store_true", help="do the work; without it only the status is shown")
    ap.add_argument("--only-head", action="store_true",
                    help="with --run: skip the intermediate commits, one step to the head of the branch")
    ap.add_argument("--max-commits", type=lambda v: v if v == "all" else int(v), metavar="N|all",
                    help="with --run: go on for at most N commits of the upstream (the one stopped at "
                         "counts; default: all)")
    ap.add_argument("--show", default="10", metavar="N|all", help="pending commits in the status (default 10)")
    ap.add_argument("--bench", action="store_true",
                    help="with --run: measure the steps in the bench (--bench-root); off by default")
    ap.add_argument("--skip-bench", action="store_true",
                    help="with --run, only after a stop at the bench of a commit: marks the commit with "
                         "%s and goes on without its measurement" % SKIP_TRAILER)
    ap.add_argument("--fetch", action="store_true", help="git fetch the upstream first")
    ap.add_argument("--python", default=sys.executable, help="for yamake2gn.py and autofix.py")
    ap.add_argument("--script-executable", default=os.environ.get("GN_SCRIPT_EXECUTABLE", "python3.9"),
                    help="gn gen --script-executable")
    c = ap.add_argument_group("remembered in <state>/config.json (given: overrides and is saved)")
    c.add_argument("--out", help="the build dir (default: <root>/out/gn)")
    c.add_argument("--env", action="append", metavar="NAME=VALUE",
                   help="for gn gen and ninja, e.g. the compiler cache's (repeatable)")
    c.add_argument("-j", "--jobs", type=int, help="ninja -j")
    c.add_argument("--nice", type=int, metavar="N",
                   help="niceness of the run and every command it starts (default: %d, 0: off)"
                        % CONFIG_DEFAULTS["nice"])
    c.add_argument("--targets", nargs="+", help="default: %s" % " ".join(DEFAULT_TARGETS))
    c.add_argument("--gen-args", help="of yamake2gn.py (default: %s)" % DEFAULT_GEN_ARGS)
    c.add_argument("--push", metavar="REMOTE", help="push the branch after every step")
    c.add_argument("--bench-root", help="the bench work tree (a worktree of this repo): measure every step")
    c.add_argument("--bench-out", help="the bench build dir (default: <bench root>/out/gn)")
    c.add_argument("--ya-out", help="ya make -o of the bench")
    c.add_argument("--ya-cache-root", help="YA_CACHE_DIR of the bench (default: ~/.ya)")
    c.add_argument("--ya-cache-size", help="ya make --cache-size of the bench, e.g. 120GiB")
    c.add_argument("--ya-dirs", nargs="+",
                   help="ya make of the bench, besides the targets follow_select.py picks for the step "
                        "(default: %s)" % " ".join(DEFAULT_YA_DIRS))
    c.add_argument("--no-select", action="store_const", const=True, help="ya builds only --ya-dirs")
    c.add_argument("--bench-env", action="append", metavar="NAME=VALUE",
                   help="for gn gen and ninja of the bench, e.g. its compiler cache tag (repeatable)")
    c.add_argument("--bench-targets", nargs="+", help="of the bench, ninja and ya (default: --targets)")
    args = ap.parse_args()

    top = subprocess.run(["git", "-C", args.root or os.getcwd(), "rev-parse", "--show-toplevel",
                          "--absolute-git-dir"], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         universal_newlines=True)
    if top.returncode != 0:
        ap.error("run it in the work tree of the GN branch (or give --root): %s" % top.stderr.strip())
    args.root, git_dir = top.stdout.split("\n")[:2]
    args.upstream = resolve_upstream(ap, args)
    if args.fetch:
        if "/" not in args.upstream:
            ap.error("--fetch needs an upstream like <remote>/<branch>: %s" % args.upstream)
        fetch_upstream(Git(args.root), args.upstream, True)
        args.fetch = False      # done here, before the status or the run touches the ref
    require_ref(ap, args.root, args.upstream)
    base = os.path.realpath(args.state or os.path.join(git_dir, "gn-follow"))
    if not args.state:
        base = os.path.join(base, state_name(args.upstream))
    args.state = base
    os.makedirs(args.state, exist_ok=True)
    cfg, given = load_config(args, os.path.join(args.state, "config.json"))
    if not args.out:
        args.out = os.path.join(args.root, "out", "gn")
    if args.bench_root and not args.bench_out:
        args.bench_out = os.path.join(args.bench_root, "out", "gn")
    validate(ap, args)
    save_config(os.path.join(args.state, "config.json"), cfg, given)

    if not args.run:
        try:
            return status(args)
        except Stop as s:
            print("\nSTOP %s\n%s" % (s.kind, s.text), file=sys.stderr)
            return EXIT[s.kind]
    if args.nice:
        os.nice(args.nice)      # the children inherit it: the build runs at low priority
    lock = open(os.path.join(args.state, "lock"), "w")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        sys.exit("follow.py: another run holds %s/lock" % args.state)
    log = open(os.path.join(args.state, "run.log"), "a")
    term = sys.stdout
    sys.stdout, sys.stderr = Tee(sys.stdout, log), Tee(sys.stderr, log)
    print("\n%s follow.py %s" % (time.strftime("%Y-%m-%d %H:%M:%S"), " ".join(sys.argv[1:])))
    f = Follower(args, term)
    try:
        rc = f.follow()
    except Stop as s:
        print("\nSTOP %s\n%s" % (s.kind, s.text), file=sys.stderr)
        rc = EXIT[s.kind]
    print("exit: %d" % rc)
    return rc


if __name__ == "__main__":
    sys.exit(main())
