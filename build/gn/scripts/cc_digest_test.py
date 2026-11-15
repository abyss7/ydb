import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
from unittest.mock import patch

from cc_digest import digest, signature

FLAGS = "clang++ -I../.. -Igen -D_GNU_SOURCE -std=c++20 -fPIC"


def entry(src, obj, flags=FLAGS, extra=""):
    return {
        "directory": "out/gn",
        "file": src,
        "command": "%s %s -MD -MF obj/%s.d -Xclang -include -Xclang gen/x_slot_weak.h"
                   " -c %s -o obj/%s" % (flags, extra, obj, src, obj),
    }


class SignatureTest(unittest.TestCase):
    def test_a_new_source_of_the_same_flags_changes_nothing(self):
        self.assertEqual(signature(entry("a.cpp", "a.o")["command"]),
                         signature(entry("b.cpp", "b.o")["command"]))
        self.assertEqual(digest([entry("a.cpp", "a.o")]),
                         digest([entry("a.cpp", "a.o"), entry("b.cpp", "b.o")]))

    def test_a_new_flag_changes_it(self):
        self.assertNotEqual(signature(entry("a.cpp", "a.o")["command"]),
                            signature(entry("a.cpp", "a.o", extra="-DTSTRING_IS_STD_STRING")["command"]))
        self.assertNotEqual(signature(entry("a.cpp", "a.o")["command"]),
                            signature(entry("a.cpp", "a.o", flags=FLAGS + " -DNDEBUG")["command"]))


class MainTest(unittest.TestCase):
    def run_main(self, d, entries):
        cc = os.path.join(d, "compile_commands.json")
        out = os.path.join(d, "compile_commands.digest")
        dep = os.path.join(d, "compile_commands.digest.d")
        with open(cc, "w") as f:
            json.dump(entries, f)
        with patch.object(sys, "argv", ["cc_digest.py", "--compile-commands", cc,
                                        "--output", out, "--depfile", dep]):
            with contextlib.redirect_stdout(io.StringIO()):
                import cc_digest
                cc_digest.main()
        with open(dep) as f:
            self.assertEqual(f.read(), "%s: %s\n" % (out, cc))
        with open(out) as f:
            return out, f.read()

    def test_an_unchanged_digest_keeps_its_mtime(self):
        with tempfile.TemporaryDirectory() as d:
            out, text = self.run_main(d, [entry("a.cpp", "a.o")])
            before = os.stat(out).st_mtime_ns
            # a new source of the same flags: same digest, same mtime -- the
            # weak actions taking it as an input stay clean
            _, text2 = self.run_main(d, [entry("a.cpp", "a.o"), entry("b.cpp", "b.o")])
            self.assertEqual(text, text2)
            self.assertEqual(os.stat(out).st_mtime_ns, before)
            # a new flag: the digest changes
            _, text3 = self.run_main(d, [entry("a.cpp", "a.o", extra="-DX")])
            self.assertNotEqual(text, text3)


if __name__ == "__main__":
    unittest.main()
