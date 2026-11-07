import os
import shlex
import tempfile
import unittest

from link_slot_weak import compile_argv


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
