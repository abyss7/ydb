"""Small GN/PCH workflow contract test, not a performance benchmark.

Usage: python3 link_workflow_test.py /path/to/pch_update /path/to/clang++
Requires GN, Ninja, readelf, nm and lld. Creates a temporary Git repository
with one fixture commit, four build targets and a tiny header. Uses fixed
baseline trace/log costs to exercise selection above the production threshold;
a compiler proxy adds a small fixed no-PCH cost for calibration. Compilation,
GN generation, symbol discovery, compatibility, export and links are real.
Retains the fixture/log on failure; removes only its own scratch on success.
"""

import csv
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    executable, compiler = (str(Path(p).resolve()) for p in sys.argv[1:])
    source = Path(__file__).resolve().parents[3]
    scratch = Path(tempfile.mkdtemp(prefix="pch-link-workflow-"))
    root, baseline = scratch / "source", scratch / "baseline"
    root.mkdir()
    binary = scratch / "pch_update"
    shutil.copy2(executable, binary)
    library_path = os.pathsep.join(filter(None, (
        str(baseline), str(Path(executable).parent), os.environ.get("LD_LIBRARY_PATH", ""))))
    gn, ninja = shutil.which("gn"), shutil.which("ninja")
    assert gn and ninja, "GN and Ninja must be on PATH"
    log = (scratch / "workflow.log").open("w")
    print("fixture:", scratch, flush=True)

    def run(args, cwd=root):
        args = list(map(str, args))
        print("RUN", args, file=log, flush=True)
        result = subprocess.run(args, cwd=cwd, stdout=log, stderr=subprocess.STDOUT,
                                env=dict(os.environ, LD_LIBRARY_PATH=library_path))
        log.flush()
        if result.returncode:
            raise RuntimeError("exit %d: %s; see %s" % (result.returncode, args, log.name))

    def write(path, text):
        path = root / path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    # Keep real compiler behavior, but make the no-PCH/PCH cost contrast stable
    # on tiny sources. These delays and the baseline costs below are fixtures,
    # not measurements of a PCH performance benefit.
    cxx = scratch / "clang++"
    cxx.write_text("#!" + sys.executable + "\n" +
        "import subprocess, sys, time\n"
        "args = sys.argv[1:]\n"
        "result = subprocess.run([" + repr(compiler) + "] + args)\n"
        "if '-c' in args and '-include-pch' not in args and any(a.endswith('.cpp') for a in args):\n"
        "    time.sleep(0.05)\n"
        "sys.exit(result.returncode)\n")
    cxx.chmod(0o755)

    # Apply builds a separate global directory and deletes it on success.
    # Inspect and run its actual results before cleanup, not the no-PCH baseline.
    proxy = scratch / "ninja-proxy"
    applied = scratch / "applied-commands.json"
    proxy.write_text("#!" + sys.executable + "\n" +
        "import json, os, pathlib, subprocess, sys\n"
        "args = sys.argv[1:]\n"
        "subprocess.run([" + repr(ninja) + "] + args, check=True)\n"
        "if '-C' in args and '-t' not in args and '-n' not in args:\n"
        "    build = pathlib.Path(args[args.index('-C') + 1])\n"
        "    if build.name == 'global':\n"
        "        commands = (build / 'compile_commands.json').read_text()\n"
        "        pathlib.Path(" + repr(str(applied)) + ").write_text(commands)\n"
        "        env = dict(os.environ, LD_LIBRARY_PATH=str(build) + os.pathsep + os.environ.get('LD_LIBRARY_PATH', ''))\n"
        "        for name in ('aaa_control', 'allowed', 'denied'):\n"
        "            subprocess.run([str(build / name)], cwd=build, env=env, check=True)\n")
    proxy.chmod(0o755)

    write(".gn", 'buildconfig = "//config.gn"\n')
    write("config.gn", '''declare_args() {
  pch_mode = "none"
  time_trace = false
}
set_default_toolchain("//toolchain:clang")
platform_deps = []
import("//build/gn/pch/global/groups.gni")
foreach(kind, [ "source_set", "shared_library", "executable" ]) {
  set_defaults(kind) { configs = [ "//:common" ] }
}
''')
    # Copy production solink verbatim: its distinct link_output/depend_output
    # contract must not be simplified into a hand-written Ninja graph.
    production = (source / "build/gn/toolchain/linux/BUILD.gn").read_text()
    solink = production.split('    tool("solink") {', 1)[1].split('\n    tool("link") {', 1)[0]
    write("toolchain/BUILD.gn", '''pool("compile_pool") { depth = 2 }
toolchain("clang") {
  ld = "%s -fuse-ld=lld"
  sysroot = ""
  tool("cxx") {
    pool = ":compile_pool"
    command = "%s -MD -MF {{output}}.d {{defines}} {{include_dirs}} {{cflags}} {{cflags_cc}} -c {{source}} -o {{output}}"
    depfile = "{{output}}.d"
    depsformat = "gcc"
    outputs = [ "{{source_out_dir}}/{{target_output_name}}.{{source_file_part}}.o" ]
    description = "CXX {{output}}"
  }
  tool("solink") {%s
  tool("link") {
    command = "$ld {{ldflags}} -o {{output}} {{inputs}} {{solibs}} {{libs}}"
    outputs = [ "{{root_out_dir}}/{{target_output_name}}{{output_extension}}" ]
    description = "LINK {{output}}"
  }
  tool("stamp") { command = "touch {{output}}" }
  tool("copy") { command = "cp {{source}} {{output}}" }
}
''' % (cxx, cxx, solink))
    for name in ("BUILD.gn", "pch_flags.gni"):
        write("build/gn/pch/global/" + name, (source / "build/gn/pch/global" / name).read_text())
    write("build/gn/pch/global/groups.gni", "pch_groups = []\n")
    write("build/gn/pch/global/enum_serialization.gni", "enum_serialization_pch_exclude = []\n")
    write("build/gn/scripts/pch_check.py", (source / "build/gn/scripts/pch_check.py").read_text())
    write("contrib/libs/cxxsupp/libcxx/BUILD.gn", 'group("libcxx") {}\n')
    write("provider.cpp", 'extern "C" int Provided() { return 7; }\n')
    write("header.h", '''#pragma once
#ifndef SKIP_REGISTRATION
extern "C" int Provided();
static const int Registration = Provided();
#endif
struct Value { int x; };
''')
    build = '''config("common") {
  cflags = [ "-O2", "-fPIC" ]
  cflags_cc = [ "-std=c++20" ]
  include_dirs = [ "//" ]
  if (time_trace) { cflags_cc += [ "-ftime-trace" ] }
}
shared_library("provider") { sources = [ "provider.cpp" ] }
'''
    consumers = set()
    # Target0 is intentionally held out from initial PCH assignment by Select.
    for name in ("aaa_control", "allowed", "denied"):
        files = [name + "0.cpp", name + "1.cpp"]
        consumers.update(files)
        for i, path in enumerate(files):
            write(path, ("#define SKIP_REGISTRATION\n" if name == "denied" else "") +
                  '#include "header.h"\n' +
                  ("int main() { return 0; }\n" if i == 0 else "int Size() { return sizeof(Value); }\n"))
        build += '''executable("%s") {
  sources = %s
  deps = %s
  metadata = { global_pch_owner = [ "//:%s" ] }
  if (pch_mode == "global") {
    foreach(g, pch_groups) {
      foreach(owner, g.targets) {
        if (owner == "//:%s") { deps += [ "//build/gn/pch/global:pch_${g.name}" ] }
      }
    }
  }
}
''' % (name, json.dumps(files), json.dumps([":provider"] if name != "denied" else []), name, name)
    build += 'group("all") { deps = [ ":aaa_control", ":allowed", ":denied" ] }\n'
    build += 'group("tests") { deps = [ ":all" ] }\n'
    write("BUILD.gn", build)
    # User-authorized fixture-only commit; no references to the working repo's
    # index/history, no global configuration, no hooks or signing.
    run(["git", "init", "--quiet"])
    run(["git", "add", "."])
    run(["git", "-c", "user.name=PCH fixture", "-c", "user.email=pch-fixture@invalid",
         "-c", "core.hooksPath=/dev/null", "-c", "commit.gpgsign=false", "commit", "--quiet", "-m", "Fixture"])
    run([gn, "gen", baseline, "--root=" + str(root), "--script-executable=" + sys.executable,
         '--args=pch_mode="none" time_trace=true', "--export-compile-commands"])
    run([ninja, "-C", baseline, "all", "tests"])
    assert (baseline / "libprovider.so.TOC").is_file()
    assert not (baseline / "libprovider.so.TOC").read_bytes().startswith(b"\x7fELF")
    manifest = (baseline / "obj/allowed.ninja").read_text()
    assert "libprovider.so.TOC" in manifest and "solibs =" in manifest, manifest
    for name in ("aaa_control", "allowed", "denied"):
        run([baseline / name], cwd=baseline)

    # Deterministic model INPUTS, not measured performance: each of six source
    # files spends 10s in header.h out of 12s total. No expensive C++ needed.
    objects = set()
    for row in json.loads((baseline / "compile_commands.json").read_text()):
        name = Path(row["file"]).name
        if name not in consumers:
            continue
        owner = name[:-5]  # remove the TU digit and .cpp
        obj = "obj/%s.%s.o" % (owner, name)
        objects.add(obj)
        trace = baseline / (obj[:-2] + ".json")
        trace.write_text(json.dumps({"traceEvents": [
            {"pid": 1, "tid": 1, "ph": "X", "name": "Source", "ts": 1000, "dur": 10000000,
             "args": {"detail": "../source/header.h"}},
            {"pid": 1, "tid": 1, "ph": "X", "name": "ExecuteCompiler", "ts": 0, "dur": 12000000},
            {"pid": 1, "tid": 1, "ph": "X", "name": "Total Frontend", "ts": 0, "dur": 11000000},
        ]}))
    assert len(objects) == 6, objects
    ninja_log = baseline / ".ninja_log"
    rows = []
    for line in ninja_log.read_text().splitlines():
        fields = line.split("\t")
        if len(fields) == 5 and fields[3] in objects:
            fields[1] = str(int(fields[0]) + 12000)
        rows.append("\t".join(fields))
    ninja_log.write_text("\n".join(rows) + "\n")
    print("fixture costs: six TUs, 10s header/12s compile (synthetic model input)", flush=True)

    common = [binary, "--baseline", baseline, "--gn", gn, "--ninja", proxy]
    run(common + ["--predict", "--disk", "1G", "--q", "1", "--sample", "100", "--main-ref", "HEAD",
                  "--workers", "2", "--analysis-jobs", "2", "--ram-gb", "2", "--action-memory-gb", "0.5"])
    cycle = baseline / ".pch_update/current"

    def check_plan(phase):
        plan = cycle / phase
        with (plan / "assignments.tsv").open() as rows:
            assignments = {row["target"]: row for row in csv.DictReader(rows, delimiter="\t")}
        assert int(assignments["//:allowed"]["leaf"]) >= 0, assignments
        assert int(assignments["//:denied"]["leaf"]) == -1, assignments
        with (plan / "compatibility.tsv").open() as rows:
            checks = list(csv.DictReader(rows, delimiter="\t"))
        denied = next(row for row in checks if row["kind"] == "target" and row["target_or_group"] == "//:denied")
        assert int(denied["old_leaf"]) >= 0 and int(denied["new_leaf_before_compaction"]) == -1, denied
        assert "no known provider" in denied["diagnostics"] and "denied" in denied["diagnostics"], denied
        assert any(row["kind"] == "pch-link" and int(row["new_leaf_before_compaction"]) >= 0 for row in checks), checks
        assert any("header.h" in p.read_text() for p in (plan / "build/pch").glob("*.h")), "selected PCH lost its tested header"
        print("PASS:", phase, "retains allowed PCH and rejects denied at final link", flush=True)

    check_plan("initial")
    run(common + ["--correct"])
    check_plan("corrected")
    run(common + ["--apply"])
    commands = json.loads(applied.read_text())
    allowed = [row for row in commands if Path(row["file"]).name.startswith("allowed")]
    denied = [row for row in commands if Path(row["file"]).name.startswith("denied")]
    assert len(allowed) == 2 and all("-include-pch" in row["command"] for row in allowed), allowed
    assert len(denied) == 2 and all("-include-pch" not in row["command"] for row in denied), denied
    assert not (baseline / ".pch_update").exists(), "successful workflow left disposable state"
    print("PASS: real GN TOC, extraction, selection, fallback, correction, PCH export and final executable links/runs", flush=True)
    log.close()
    shutil.rmtree(scratch)


if __name__ == "__main__":
    main()
