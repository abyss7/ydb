import os
import tempfile
import types
import unittest
from unittest.mock import patch

import yamake2gn


def read(path):
    with open(path) as f:
        return f.read()


class GlobalPchPruneTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = self.tmp.name
        self.mods = {"src": types.SimpleNamespace(kind="LIBRARY")}
        self.generated = {"src/output.h": "src"}
        self.external = yamake2gn.ExternalTargets(self.root)
        self.pch = "build/gn/pch/global/group_1/pch.cc"

    def put(self, path, text):
        full = os.path.join(self.root, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w") as f:
            f.write(text)

    def plan(self):
        return yamake2gn.prune_global_pch(self.root, self.mods, self.generated, self.external)

    def test_deleted_and_moved_headers_are_removed_without_replacement(self):
        self.put("src/current.h", "")
        self.put("src/new/moved.h", "")
        original = '#pragma once\n#include "src/old.h"\n#include "src/moved.h"\n#include "src/current.h"\n'
        self.put(self.pch, original)
        per_target = "build/gn/pch/per_target/src/src.h"
        self.put(per_target, '#include "src/old.h"\n')
        changes, removed = self.plan()
        self.assertEqual(changes[self.pch], '#pragma once\n#include "src/current.h"\n')
        self.assertEqual(removed, [(self.pch, "src/old.h"), (self.pch, "src/moved.h")])
        # Planning (also used by dry-run) never mutates either mode.
        self.assertEqual(read(os.path.join(self.root, self.pch)), original)
        self.assertEqual(read(os.path.join(self.root, per_target)), '#include "src/old.h"\n')
        for path, text in changes.items():
            yamake2gn.write_if_changed(os.path.join(self.root, path), text)
        self.assertEqual(self.plan(), ({}, []))

    def test_generated_and_unmodelled_headers_survive_without_build_dir(self):
        self.put("src/custom/BUILD.gn", '# yamake2gn: keep\n')
        headers = ["src/output.h", "src/message.pb.h", "src/nodes.gen.h",
                   "src/generated/plain.h", "src/custom/plain.h", "unknown/plain.h",
                   "contrib/lib/plain.h", "../relative.h", "/absolute.h", "pch.h"]
        text = "".join('#include "%s"\n' % h for h in headers) + "#include <vector>\n"
        self.put(self.pch, text)
        self.assertEqual(self.plan(), ({}, []))

    def test_conditional_or_commented_lists_are_not_rewritten(self):
        for text in ('#if 0\n#include "src/old.h"\n#endif\n',
                     '/*\n#include "src/old.h"\n*/\n'):
            self.put(self.pch, text)
            self.assertEqual(self.plan(), ({}, []))


class TreeScanTest(unittest.TestCase):
    """find_yamakes() and find_variant_incs() share one os.walk of the tree."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.addCleanup(yamake2gn._tree_scan_cache.clear)
        self.root = os.path.realpath(self.tmp.name)
        for rel in ("a/ya.make", "a/b/ya.make", "a/b/ya.make.inc", "c/ya.make.inc",
                    "contrib/x/ya.make", "contrib/x/BUILD.gn", "out/gn/ya.make"):
            path = os.path.join(self.root, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            open(path, "w").close()
        self.build = os.path.join(self.root, "out", "gn")
        self.addCleanup(setattr, yamake2gn, "_build_dir", None)
        yamake2gn._build_dir = os.path.join(self.root, "out", "gn")

    def test_yamakes_skip_contrib_and_the_build_dir(self):
        self.assertEqual(sorted(yamake2gn.find_yamakes(self.root, self.root)),
                         ["a", os.path.join("a", "b")])

    def test_variant_incs_of_the_whole_tree(self):
        self.assertEqual(sorted(yamake2gn.find_variant_incs(self.root, ["."])),
                         [os.path.join("a", "b", "ya.make.inc"), "c/ya.make.inc"])

    def test_one_walk_for_both(self):
        with patch.object(yamake2gn.os, "walk", wraps=os.walk) as walk:
            list(yamake2gn.find_yamakes(self.root, self.root))
            list(yamake2gn.find_variant_incs(self.root, ["."]))
        self.assertEqual(walk.call_count, 1)

    def test_a_subtree_still_walks_itself(self):
        self.assertEqual(sorted(yamake2gn.find_yamakes(self.root, os.path.join(self.root, "a"))),
                         ["a", os.path.join("a", "b")])


class IntoModuleTest(unittest.TestCase):
    """`# gn: into <dir>[:<part>]` of the whole target: every source of the
    module goes there, so a source added later needs no comment of its own."""

    def parse(self, srcs, directive):
        with tempfile.TemporaryDirectory() as tmp:
            root = os.path.realpath(tmp)
            directory = os.path.join(root, "a")
            os.makedirs(directory)
            path = os.path.join(directory, "ya.make")
            with open(path, "w") as f:
                f.write("%s\nLIBRARY()\n\nSRCS(\n%s)\n\nEND()\n" % (directive, srcs))
            return yamake2gn.parse_yamake(path, "a", root)

    def test_every_source_goes_to_the_named_target(self):
        self.assertEqual(self.parse("    x.cpp\n    y.h\n", "# gn: into b").src_into,
                         {"x.cpp": ("b", None), "y.h": ("b", None)})

    def test_a_part_of_the_target(self):
        self.assertEqual(self.parse("    x.cpp\n", "# gn: into b:p").src_into,
                         {"x.cpp": ("b", "p")})

    def test_a_source_saying_its_own_target_wins(self):
        self.assertEqual(self.parse("    x.cpp\n    y.cpp  # gn: into c\n",
                                    "# gn: into b").src_into,
                         {"x.cpp": ("b", None), "y.cpp": ("c", None)})


class PeerdirPolicyTest(unittest.TestCase):
    def test_incoming_policy_does_not_block_conversion_or_add_dependencies(self):
        with tempfile.TemporaryDirectory() as root:
            directory = os.path.join(root, "a")
            os.makedirs(directory)
            path = os.path.join(directory, "ya.make")
            for kind, source in (("LIBRARY", "a.cpp"), ("PROTO_LIBRARY", "a.proto")):
                with self.subTest(kind=kind):
                    with open(path, "w") as f:
                        f.write(
                            f"{kind}()\n"
                            f"SRCS({source})\n"
                            "PEERDIR(existing/dep)\n"
                            "PEERDIR_FROM_POLICY(ALLOW allowed/consumer)\n"
                            "PEERDIR_FROM_POLICY(DENY .*)\n"
                            "END()\n"
                        )
                    mod = yamake2gn.parse_yamake(path, "a", root)
                    self.assertTrue(yamake2gn.is_trivial(mod), yamake2gn.nontrivial_macros(mod))
                    self.assertEqual(mod.peerdirs, ["existing/dep"])


class WriteBuildgnsTest(unittest.TestCase):
    """write_buildgns() formats and writes only what it did not write before."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = self.tmp.name
        self.cache = os.path.join(self.root, "out", "gn", "yamake2gn_render.json")
        self.formatted = []
        patches = [
            patch.object(yamake2gn, "gn_version", return_value="test"),
            patch.object(yamake2gn, "format_buildgns", side_effect=self.format),
            patch.object(yamake2gn, "normalize_dep_lists", side_effect=lambda t: t),
        ]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)
        self.a = os.path.join(self.root, "a", "BUILD.gn")
        self.b = os.path.join(self.root, "b", "BUILD.gn")
        os.makedirs(os.path.dirname(self.a))
        os.makedirs(os.path.dirname(self.b))

    def format(self, root, texts):
        self.formatted.append(list(texts))
        return [t + "\n# laid out\n" for t in texts]

    def write(self, **texts):
        self.formatted = []
        yamake2gn.write_buildgns(self.root, texts, False, self.cache)

    def test_the_same_render_is_written_and_formatted_once(self):
        self.write(**{self.a: "a"})
        self.assertEqual(read(self.a), "a\n# laid out\n")
        self.write(**{self.a: "a"})
        self.assertEqual(self.formatted, [])
        self.assertEqual(read(self.a), "a\n# laid out\n")

    def test_a_changed_render_is_formatted_again(self):
        self.write(**{self.a: "a", self.b: "b"})
        self.write(**{self.a: "a", self.b: "b2"})
        self.assertEqual(self.formatted, [["b2"]])
        self.assertEqual(read(self.b), "b2\n# laid out\n")

    def test_a_file_edited_by_hand_is_put_back(self):
        self.write(**{self.a: "a"})
        with open(self.a, "w") as f:
            f.write("hand written")
        self.write(**{self.a: "a"})
        self.assertEqual(len(self.formatted), 1)
        self.assertEqual(read(self.a), "a\n# laid out\n")

    def test_a_new_file_is_formatted(self):
        self.write(**{self.a: "a"})
        self.write(**{self.a: "a", self.b: "b"})
        self.assertEqual(self.formatted, [["b"]])

    def test_another_gn_formats_everything_again(self):
        self.write(**{self.a: "a"})
        with patch.object(yamake2gn, "gn_version", return_value="other"):
            self.write(**{self.a: "a"})
        self.assertEqual(self.formatted, [["a"]])

    def test_no_cache_means_always_format(self):
        yamake2gn.write_buildgns(self.root, {self.a: "a"}, False, None)
        yamake2gn.write_buildgns(self.root, {self.a: "a"}, False, None)
        self.assertEqual(len(self.formatted), 2)

    def test_no_temporary_file_is_left(self):
        self.write(**{self.a: "a"})
        leftovers = [f for f in os.listdir(os.path.dirname(self.cache)) if f.endswith(".tmp")]
        self.assertEqual(leftovers, [])


if __name__ == "__main__":
    unittest.main()
