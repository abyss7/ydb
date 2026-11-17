"""Native regressions for post_compile.py: PCH imports, paths and Ninja depfiles.

PCH_TEST_CLANG=/path/to/clang++ python3 build/gn/scripts/post_compile_test.py
"""

import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest


SCRIPTS = Path(__file__).resolve().parent
CLANG = os.environ.get("PCH_TEST_CLANG", "clang++")


class PostCompileTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="pch-compile-")
        self.addCleanup(self.tmp.cleanup)
        self.work = Path(self.tmp.name)

    def prepare(self, layout):
        root = self.work / "source tree"
        build = {"root": root, "nested": root / "out/gn",
                 "external": self.work / "external build"}[layout]
        (root / "include").mkdir(parents=True)
        (build / "gen").mkdir(parents=True)
        (build / "obj").mkdir()
        (root / "include/base.h").write_text(
            "#pragma once\nstruct Base { int value; };\n")
        (build / "gen/forced.h").write_text("#pragma once\n#define FORCED 7\n")
        (root / "base.cc").write_text("#include <base.h>\n")
        (root / "middle.cc").write_text("#pragma once\nstruct Middle { Base base; };\n")
        (root / "leaf.cc").write_text("#pragma once\nstruct Leaf { Middle middle; };\n")
        (root / "main.cc").write_text(
            "static_assert(FORCED == 7);\nint value() { return sizeof(Leaf); }\n")
        (root / "ordinary.cc").write_text(
            "#include <base.h>\nstatic_assert(FORCED == 7);\nint value() { return sizeof(Base); }\n")
        return root, build

    def compile(self, root, build, source, output, parent=None, wrapped=True, check=True, wrapper=None):
        command = [CLANG, "-std=c++20", "-I" + os.path.relpath(root / "include", build),
                   "-isysroot", str(root) + "/", "-ffile-prefix-map=" + str(root) + "/=",
                   "-Xclang", "-fno-pch-timestamp", "-Xclang", "-module-file-deps",
                   "-Xclang", "-include", "-Xclang", "gen/forced.h",
                   "-MD", "-MF", output + ".d"]
        if output.endswith(".pch"):
            command += ["-x", "c++-header", "-relocatable-pch",
                        "-Xclang", "-fmodule-file-home-is-cwd"]
        if parent:
            command += ["-include-pch", parent]
        command += ["-c", os.path.relpath(root / source, build), "-o", output]
        if wrapped:
            command = ([sys.executable, str(SCRIPTS / "post_compile.py"), str(root), "--global-pch"]
                       + (wrapper or []) + ["--"] + command)
        result = subprocess.run(command, cwd=str(build), text=True, capture_output=True)
        if check:
            self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def chain(self, root, build, wrapped=True):
        self.compile(root, build, "base.cc", "gen/base.pch", wrapped=wrapped)
        self.compile(root, build, "middle.cc", "gen/middle.pch", "gen/base.pch", wrapped=wrapped)
        self.compile(root, build, "leaf.cc", "gen/leaf.pch", "gen/middle.pch", wrapped=wrapped)

    def check_deps(self, build):
        text = (build / "obj/main.o.d").read_text().replace("\\\n", "")
        target, deps = text.split(":", 1)
        self.assertEqual(target, "obj/main.o")
        paths = shlex.split(deps)
        for pch in ("gen/base.pch", "gen/middle.pch", "gen/leaf.pch"):
            self.assertIn(pch, paths)
        for path in paths:
            self.assertTrue((build / path).is_file(), path)

    def test_source_root_build(self):
        self.check_layout("root")

    def test_nested_build(self):
        self.check_layout("nested")

    def test_external_build(self):
        self.check_layout("external")

    def check_layout(self, layout):
        root, build = self.prepare(layout)
        self.chain(root, build)
        self.compile(root, build, "main.cc", "obj/main.o", "gen/leaf.pch")
        self.check_deps(build)

    def test_nested_import_regression(self):
        root, build = self.prepare("nested")
        self.compile(root, build, "base.cc", "gen/base.pch", wrapped=False)
        self.compile(root, build, "middle.cc", "gen/middle.pch", "gen/base.pch", wrapped=False)
        result = self.compile(root, build, "main.cc", "obj/before.o", "gen/middle.pch",
                              wrapped=False, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("PCH file 'out/gn/gen/base.pch' not found", result.stderr)
        self.chain(root, build)
        self.compile(root, build, "main.cc", "obj/main.o", "gen/leaf.pch")
        self.check_deps(build)

    def test_relocate_tree(self):
        root, build = self.prepare("nested")
        self.chain(root, build)
        moved = self.work / "relocated source"
        root.rename(moved)
        self.compile(moved, moved / "out/gn", "main.cc", "obj/main.o", "gen/leaf.pch")
        self.check_deps(moved / "out/gn")

    def test_missing_parent_is_not_hidden(self):
        root, build = self.prepare("nested")
        self.chain(root, build)
        (build / "gen/base.pch").unlink()
        result = self.compile(root, build, "main.cc", "obj/main.o", "gen/leaf.pch", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("base.pch' not found", result.stderr)

    def test_no_pch_keeps_cwd_and_arguments(self):
        probe = self.work / "probe.py"
        probe.write_text("import json,os,sys; print(json.dumps([os.getcwd(),sys.argv[1:]]))")
        args = ["-c", "relative.cc", "-Irelative", "-DVALUE=unchanged"]
        output = subprocess.check_output(
            [sys.executable, str(SCRIPTS / "post_compile.py"), "/not/a/real/root", "--global-pch", "--",
             sys.executable, str(probe)] + args, cwd=str(self.work), text=True)
        self.assertEqual(json.loads(output), [str(self.work), args])

    def test_no_pch_depfile(self):
        root, build = self.prepare("nested")
        self.compile(root, build, "ordinary.cc", "obj/ordinary.o")
        target, deps = (build / "obj/ordinary.o.d").read_text().replace("\\\n", "").split(":", 1)
        self.assertEqual(target, "obj/ordinary.o")
        paths = shlex.split(deps)
        self.assertIn("gen/forced.h", paths)
        self.assertIn("../../include/base.h", paths)
        for path in paths:
            self.assertTrue((build / path).is_file(), path)

    def test_compiler_wrapper_runs_from_pch_root(self):
        root, build = self.prepare("nested")
        self.chain(root, build)
        wrapper = self.work / "forward.py"
        log = self.work / "wrapper.json"
        wrapper.write_text(
            "import json,os,sys\n"
            "with open(sys.argv[1], 'w') as f: json.dump([os.getcwd(),sys.argv[2:]],f)\n"
            "os.execvp(sys.argv[2],sys.argv[2:])\n")
        self.compile(root, build, "main.cc", "obj/main.o", "gen/leaf.pch",
                     wrapper=[sys.executable, str(wrapper), str(log)])
        cwd, argv = json.loads(log.read_text())
        self.assertEqual(cwd, str(root))
        self.assertEqual(argv[0], CLANG)
        self.assertEqual(argv[argv.index("-include-pch") + 1], "out/gn/gen/leaf.pch")
        self.check_deps(build)


if __name__ == "__main__":
    unittest.main()
