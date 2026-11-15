import contextlib
import io
import json
import os
import shlex
import sys
import tempfile
import unittest
from unittest.mock import patch

from link_slot_weak import append_dep, compile_argv, flags_signature


class AppendDepTest(unittest.TestCase):
    def test_adds_the_compile_commands_to_the_depfile(self):
        with tempfile.TemporaryDirectory() as d:
            dep = os.path.join(d, "weak.d")
            with open(dep, "w") as f:
                f.write("out.h: /a/b.h /c/d.h\n")
            append_dep(dep, "/x y/compile_commands.json")
            with open(dep) as f:
                self.assertEqual(f.read(),
                                 "out.h: /a/b.h /c/d.h /x\\ y/compile_commands.json\n")


class StampTest(unittest.TestCase):
    ENTRY = {"file": "a.cpp", "command": "clang++ -D_GNU_SOURCE -Igen -c a.cpp -o a.o"}

    def run_main(self, tmp, stamp_text, declared=None):
        header = os.path.join(tmp, "slot.h")
        with open(header, "w") as f:
            f.write("void f();\n")
        cc = os.path.join(tmp, "cc.json")
        entry = dict(self.ENTRY, directory=tmp)
        with open(cc, "w") as f:
            json.dump([entry], f)
        out, stamp, dep = (os.path.join(tmp, n) for n in ("weak.h", "weak.stamp", "weak.d"))
        with open(dep, "w") as f:
            f.write("%s: %s\n" % (out, header))
        if stamp_text is not None:
            with open(stamp, "w") as f:
                f.write(stamp_text)
        argv = ["link_slot_weak.py", "--slot", "s", "--compile-commands", cc,
                "--root", tmp, "--header", header, "--output", out,
                "--depfile", dep, "--stamp", stamp]
        import link_slot_weak
        with patch.object(sys, "argv", argv), \
             patch("link_slot_weak.declared",
                   side_effect=AssertionError("the AST dumps ran")) as dumps:
            if declared is not None:            # the caller expects them: fake them
                dumps.side_effect = None
                dumps.return_value = declared
            link_slot_weak.main()
        return out, stamp, dep, dumps

    def borrowed(self, tmp):
        directory, argv = compile_argv([dict(self.ENTRY, directory=tmp)], None, tmp)
        return flags_signature(argv)

    def test_same_borrowed_flags_skip_the_dumps(self):
        with tempfile.TemporaryDirectory() as tmp:
            out, stamp, dep, dumps = self.run_main(tmp, self.borrowed(tmp) + "\n")
            dumps.assert_not_called()
            self.assertFalse(os.path.exists(out))     # the header is not touched
            with open(dep) as f:                      # the commands stay a dependency
                self.assertEqual(f.read(), "%s: %s\n" % (out, os.path.join(tmp, "cc.json")))
            with open(stamp) as f:
                self.assertEqual(f.read(), self.borrowed(tmp) + "\n")

    def test_other_flags_run_the_dumps_and_remember_the_new_ones(self):
        with tempfile.TemporaryDirectory() as tmp:
            out, stamp, dep, dumps = self.run_main(tmp, "stale\n", declared={"_ZN1fEv"})
            dumps.assert_called_once()
            with open(out) as f:
                self.assertIn(".weak _ZN1fEv", f.read())
            with open(dep) as f:                      # the commands appended to clang's
                self.assertIn("cc.json", f.read())
            with open(stamp) as f:
                self.assertEqual(f.read(), self.borrowed(tmp) + "\n")


class CompileArgvTest(unittest.TestCase):
    def test_borrowed_generated_source_is_not_an_input(self):
        with tempfile.TemporaryDirectory() as build:
            relative = "gen/certs with spaces/certs_resources.cpp"
            absolute = os.path.join(build, relative)
            # Deliberately absent, as in a sampled build. Path matching must
            # work without building this unrelated generated translation unit.
            self.assertFalse(os.path.exists(absolute))
            flags = ["clang++", "-Igen", "-DRESOURCE=" + relative, "-std=c++20"]
            spellings = [
                (relative, absolute),
                (absolute, relative),
                (relative, "./gen/../" + relative),
                (relative, relative),
            ]
            for file, source_arg in spellings:
                argv = flags + [
                    "-c", source_arg, "-o", "certs.o", "-MD", "-MF", "certs.o.d",
                    "-include-pch", "unbuilt.pch", "-include", "unbuilt_slot.h",
                    "-Xclang", "-include", "-Xclang", "another_unbuilt_slot.h",
                ]
                for field in ("command", "arguments"):
                    with self.subTest(file=file, source_arg=source_arg, field=field):
                        entry = {"directory": build, "file": file}
                        entry[field] = " ".join(shlex.quote(a) for a in argv) if field == "command" else argv
                        directory, actual = compile_argv([entry], None, build)
                        self.assertEqual(directory, build)
                        self.assertEqual(actual, flags)

    def test_distinct_positional_input_is_not_removed(self):
        with tempfile.TemporaryDirectory() as build:
            source = "gen/a/source.cpp"
            other = "gen/b/source.cpp"
            entry = {
                "directory": build,
                "file": source,
                "arguments": ["clang++", "-c", os.path.join(build, source), other],
            }
            _, argv = compile_argv([entry], None, build)
            self.assertEqual(argv, ["clang++", other])


if __name__ == "__main__":
    unittest.main()
