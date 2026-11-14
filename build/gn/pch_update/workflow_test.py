"""Exercise the public workflow against a tiny real GN/Clang project.

Usage: python3 workflow_test.py /path/to/pch_update /path/to/clang++
Uses a sparse, shared clone of the current repository (no new commits), and
retains the fixture/log on failure. All scratch space is below TMPDIR.
"""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    binary, compiler = map(lambda p: str(Path(p).resolve()), sys.argv[1:])
    source = Path(__file__).resolve().parents[3]
    scratch = Path(tempfile.mkdtemp(prefix="pch-workflow-"))
    root = scratch / "source"
    baseline = scratch / "baseline"
    log = (scratch / "workflow.log").open("w")
    print("fixture:", scratch, flush=True)
    # Record actual Ninja invocations, including inherited resource flags.
    ninja_calls = scratch / "ninja-calls.jsonl"
    ninja_proxy = scratch / "ninja-proxy"
    ninja_binary = shutil.which("ninja")
    assert ninja_binary
    ninja_proxy.write_text("#!" + sys.executable + "\nimport json, os, sys\n"
        "with open(" + repr(str(ninja_calls)) + ", 'a') as out:\n"
        "    out.write(json.dumps(sys.argv[1:]) + '\\n')\n"
        "os.execv(" + repr(ninja_binary) + ", [" + repr(ninja_binary) + "] + sys.argv[1:])\n")
    ninja_proxy.chmod(0o755)

    def run(args, cwd=root, expected=0, env=None):
        print("RUN", args, file=log, flush=True)
        result = subprocess.run(args, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, env=env)
        log.flush()
        if result.returncode != expected:
            raise RuntimeError("unexpected exit %d: %s; see %s" % (result.returncode, args, log.name))

    def write(path, text):
        path = root / path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    run(["git", "clone", "--shared", "--no-checkout", str(source), str(root)], cwd=scratch)
    run(["git", "sparse-checkout", "set", "--no-cone", "/__pch_workflow_empty__"])
    # The cold-only fixture needs no historical workload. A local shallow
    # boundary avoids scanning the production repository's months of commits.
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True)
    (root / ".git/shallow").write_text(head)
    write(".gn", 'buildconfig = "//config.gn"\n')
    write("config.gn", '''declare_args() {
  pch_mode = "none"
  time_trace = false
}
set_default_toolchain("//toolchain:clang")
platform_deps = []
import("//build/gn/pch/global/groups.gni")
set_defaults("source_set") {
  configs = [ "//:common" ]
  deps = []
}
''')
    write("toolchain/BUILD.gn", '''toolchain("clang") {
  tool("cxx") {
    command = "%s -MD -MF {{output}}.d {{defines}} {{include_dirs}} {{cflags}} {{cflags_cc}} -c {{source}} -o {{output}}"
    depfile = "{{output}}.d"
    depsformat = "gcc"
    outputs = [ "{{source_out_dir}}/{{target_output_name}}.{{source_file_part}}.o" ]
    description = "CXX {{output}}"
  }
  tool("stamp") { command = "touch {{output}}" }
  tool("copy") { command = "cp {{source}} {{output}}" }
}
''' % compiler)
    write("contrib/libs/cxxsupp/libcxx/BUILD.gn", 'group("libcxx") {}\n')
    for name in ("BUILD.gn", "pch_flags.gni"):
        write("build/gn/pch/global/" + name, (source / "build/gn/pch/global" / name).read_text())
    write("build/gn/pch/global/groups.gni", "pch_groups = []\n")
    write("build/gn/pch/global/enum_serialization.gni", "enum_serialization_pch_exclude = []\n")
    checker = (source / "build/gn/scripts/pch_check.py").read_text()
    checker = checker.replace('if __name__ == "__main__":',
        'if __name__ == "__main__":\n'
        '    import os\n'
        '    if os.environ.get("PCH_WORKFLOW_FAIL"):\n'
        '        raise RuntimeError("injected deployment failure")\n')
    assert "injected deployment failure" in checker
    write("build/gn/scripts/pch_check.py", checker)
    write("generate.py", 'import pathlib, sys\np = pathlib.Path(sys.argv[1])\np.parent.mkdir(parents=True, exist_ok=True)\np.write_text("#pragma once\\nstruct Generated {};\\n")\n')
    write("heavy.h", '#pragma once\n#include "generated.h"\n' + "\n".join(
        "struct Type%d { int value[%d]; };" % (i, i % 13 + 1) for i in range(150000)))
    build = '''action("generate") {
  visibility = [ ":fixture_codegen" ]
  script = "generate.py"
  outputs = [ "$root_gen_dir/generated.h" ]
  args = [ rebase_path(outputs[0], root_build_dir) ]
}
group("fixture_codegen") { deps = [ ":generate" ] }
config("common") {
  cflags_cc = [ "-std=c++20" ]
  defines = [ "WORKFLOW_TEST=1" ]
  include_dirs = [ "//", "$root_gen_dir" ]
  if (time_trace) { cflags_cc += [ "-ftime-trace" ] }
}
'''
    labels = []
    for t in range(6):
        label = "lib%d" % t
        labels.append(":" + label)
        files = []
        # Header extraction requires 30s cumulative parsing. Keep ample work
        # even with an uncontended, RAM-capped -j1 baseline (no contention boost
        # to trace durations); the measured workload itself can still use PCH.
        for f in range(12):
            name = "file%d_%d.cpp" % (t, f)
            files.append(name)
            write(name, '#include "heavy.h"\nint f%d_%d() { return sizeof(Type9999); }\n' % (t, f))
        build += '''source_set("%s") {
  sources = %s
  deps = [ ":fixture_codegen" ]
  metadata = { global_pch_owner = [ "//:%s" ] }
  if (pch_mode == "global") {
    foreach(g, pch_groups) {
      foreach(owner, g.targets) {
        if (owner == "//:%s") {
          deps += [ "//build/gn/pch/global:pch_${g.name}" ]
        }
      }
    }
  }
}
''' % (label, json.dumps(files), label, label)
    build += 'group("all") { deps = %s }\ngroup("tests") { deps = [ ":all" ] }\n' % json.dumps(labels)
    write("BUILD.gn", build)
    common = [binary, "--baseline", str(baseline), "--ninja", str(ninja_proxy)]
    run(common + ["--apply"], expected=1)
    run(common + ["--predict", "--correct"], expected=1)
    run(common + ["--plan", "unused"], expected=1)
    # The fixture shares a developer host. Keep CPU monitoring enabled with an
    # explicit noise allowance; strict rejection/reset is tested by self-test.
    run(common + ["--predict", "--disk", "1G", "--q", "1", "--sample", "100", "--main-ref", "HEAD", "--workers", "2",
                  "--ram-gb", "2", "--action-memory-gb", "1.1", "--measure-max-external-cpu", "4"])
    state = baseline / ".pch_update"
    snapshots = list(state.glob("current/initial/snapshot.bin"))
    assert len(snapshots) == 1, snapshots
    cycle = snapshots[0].parent.parent
    initial_summary = dict(line.split("\t", 1) for line in (cycle / "initial/summary.tsv").read_text().splitlines()
                           if not line.startswith("#"))
    assert int(initial_summary["pchs"]) > 0, "fixture header did not reach the extraction threshold"
    frozen = snapshots[0].read_bytes()
    run(common + ["--predict"])
    assert snapshots[0].read_bytes() == frozen, "repeat predict replaced the frozen plan"
    # Simulate emission/compatibility failure after selection. Nonces and
    # available-RAM defaults may change across retries, but completed selection
    # must survive both an initial-plan retry and a correction retry.
    initial = cycle / "initial"
    chosen = (initial / "selection.bin").read_bytes()
    snapshots[0].unlink()
    report = initial / "compatibility.tsv"
    report.unlink()
    report.mkdir()

    def reused(args, expected=0):
        start = log.tell()
        run(args, expected=expected)
        text = Path(log.name).read_bytes()[start:].decode()
        assert "selection: reusing saved layout" in text, text[-2000:]
        assert "selection width=" not in text, "retry reran the selector"

    reused(common + ["--predict"], expected=1)
    report.rmdir()
    reused(common + ["--predict"])
    assert (initial / "selection.bin").read_bytes() == chosen
    corrected = cycle / "corrected"
    (corrected / "compatibility.tsv").mkdir(parents=True)
    run(common + ["--correct"], expected=1)
    chosen = (corrected / "selection.bin").read_bytes()
    reused(common + ["--correct"], expected=1)
    (corrected / "compatibility.tsv").rmdir()
    reused(common + ["--correct"])
    assert (corrected / "selection.bin").read_bytes() == chosen
    summary = dict(line.split("\t", 1) for line in (corrected / "summary.tsv").read_text().splitlines() if not line.startswith("#"))
    assert int(summary["pchs"]) > 0, "fixture must exercise actual PCH export"
    measured = {p.name: p.read_bytes() for p in (corrected / "observations").glob("*.bin")}
    run(common + ["--correct"])
    assert measured == {p.name: p.read_bytes() for p in (corrected / "observations").glob("*.bin")}
    for plan in (initial, corrected):
        assert (plan / "cpu-load.tsv").is_file(), "measured phase has no CPU monitoring report"
        assert "limit_cores=4" in (plan / "cpu-load.tsv").read_text(), "CPU allowance was not inherited"
        assert not (plan / "cpu-monitor.active").exists(), "CPU monitoring did not finish"
        assert not (plan / "cpu-rejected").exists(), "contaminated measurement was accepted"
    assert not (state / "cache/compatibility-v1").exists()
    assert not list((initial / "build").rglob("*.o")), "initial objects survived calibration"
    assert not (state / "modes").exists(), "mode-specific saved runs must not be created"
    invocations = [json.loads(line) for line in ninja_calls.read_text().splitlines()]
    baseline_builds = [a for a in invocations if "-C" in a and a[a.index("-C") + 1] == str(baseline)
                       and "-t" not in a and "-n" not in a]
    measured_builds = [a for a in invocations if "-f" in a and a[a.index("-f") + 1] == "measured.ninja"]
    assert baseline_builds and all("-j" in a and a[a.index("-j") + 1] == "1" for a in baseline_builds), baseline_builds
    assert measured_builds and all("-j" in a and a[a.index("-j") + 1] == "2" for a in measured_builds), measured_builds
    assert summary["workers"] == "2" and float(summary["ram_bytes"]) == 2e9, summary
    # A real deployment failure preserves the current measurement and restores
    # the old groups. Repair it and retry without any repeated selection/measure.
    old_groups = (root / "build/gn/pch/global/groups.gni").read_bytes()
    run(common + ["--apply"], expected=1, env=dict(os.environ, PCH_WORKFLOW_FAIL="1"))
    assert state.exists()
    assert (root / "build/gn/pch/global/groups.gni").read_bytes() == old_groups
    assert measured == {p.name: p.read_bytes() for p in (corrected / "observations").glob("*.bin")}
    run(common + ["--apply"])
    groups = (root / "build/gn/pch/global/groups.gni").read_bytes()
    assert b"compiler_flags" in groups
    assert b"//:fixture_codegen" in groups, "cheap generated header lost its public producer dependency"
    assert not state.exists() and not Path(str(state) + ".cleanup").exists(), "successful apply left scratch"
    run(common, expected=1)  # parameters do not persist between weekly runs
    run(common + ["--apply"], expected=1)
    with (root / "heavy.h").open("a") as header:
        header.write("\n// next weekly baseline\n")
    run(common + ["--disk", "1G", "--q", "1", "--sample", "100", "--main-ref", "HEAD", "--workers", "2",
                  "--ram-gb", "2", "--action-memory-gb", "1.1", "--measure-max-external-cpu", "4"])
    assert not state.exists(), "full default run must also clean all scratch"
    assert (baseline / "build.ninja").exists(), "cleanup removed the baseline"
    # Simulate SIGKILL midway through the final recursive deletion.
    garbage = Path(str(state) + ".cleanup")
    garbage.mkdir()
    (garbage / "remaining-file").write_text("partial cleanup")
    run(common)
    assert not state.exists() and not garbage.exists()
    print("PASS: one-run resume, compact scratch, exact GN apply, cleanup and fresh weekly run", flush=True)
    log.close()
    shutil.rmtree(scratch)


if __name__ == "__main__":
    main()
