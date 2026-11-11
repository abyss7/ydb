import json
import os
import tempfile
import unittest

import link_plugin_index as lpi


class CacheTest(unittest.TestCase):
    """The per-file parse kept in <build dir>/link_plugin_index.cache.json."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = os.path.join(self.tmp.name, "cache.json")
        self.stamp = ["/usr/bin/gn", 11, 22]
        self.stats = {"a/BUILD.gn": [1, 10], "b/BUILD.gn": [2, 20]}
        self.targets = {"a/BUILD.gn": [["library", "//a:a", ["//b:b"], [], []]],
                        "b/BUILD.gn": []}

    def save(self):
        lpi.save_cache(self.path, self.stamp, self.stats, self.targets)

    def stored(self):
        with open(self.path) as f:
            return json.load(f)["files"]

    def test_round_trip(self):
        self.save()
        self.assertEqual(lpi.load_cache(self.path, self.stamp, self.stats), self.targets)

    def test_saved_with_the_stat_it_was_parsed_at(self):
        self.save()
        stored = self.stored()
        self.assertEqual(sorted(stored), ["a/BUILD.gn", "b/BUILD.gn"])
        self.assertEqual(stored["a/BUILD.gn"][:2], [1, 10])
        self.assertEqual(stored["a/BUILD.gn"][2], self.targets["a/BUILD.gn"])

    def test_a_changed_stat_drops_that_one_file(self):
        self.save()
        stats = {"a/BUILD.gn": [9, 10], "b/BUILD.gn": [2, 20]}
        self.assertEqual(lpi.load_cache(self.path, self.stamp, stats), {"b/BUILD.gn": []})

    def test_a_file_that_appeared_is_not_cached(self):
        self.save()
        stats = {"a/BUILD.gn": [1, 10], "b/BUILD.gn": [2, 20], "c/BUILD.gn": [3, 30]}
        self.assertEqual(sorted(lpi.load_cache(self.path, self.stamp, stats)),
                         ["a/BUILD.gn", "b/BUILD.gn"])

    def test_another_gn_drops_everything(self):
        self.save()
        self.assertEqual(lpi.load_cache(self.path, ["/usr/bin/gn", 11, 99], self.stats), {})

    def test_another_version_drops_everything(self):
        self.save()
        with open(self.path) as f:
            data = json.load(f)
        data["version"] = lpi.CACHE_VERSION + 1
        with open(self.path, "w") as f:
            json.dump(data, f)
        self.assertEqual(lpi.load_cache(self.path, self.stamp, self.stats), {})

    def test_broken_json_is_ignored(self):
        with open(self.path, "w") as f:
            f.write("{not json")
        self.assertEqual(lpi.load_cache(self.path, self.stamp, self.stats), {})

    def test_no_cache_file(self):
        self.assertEqual(lpi.load_cache(self.path, self.stamp, self.stats), {})

    def test_files_outside_the_stat_set_are_not_stored(self):
        lpi.save_cache(self.path, self.stamp, {"a/BUILD.gn": [1, 10]}, self.targets)
        self.assertEqual(sorted(self.stored()), ["a/BUILD.gn"])

    def test_no_temporary_file_is_left(self):
        self.save()
        self.assertEqual([f for f in os.listdir(self.tmp.name) if f.endswith(".tmp")], [])

    def test_no_build_dir_saves_nothing(self):
        lpi.save_cache(None, self.stamp, self.stats, self.targets)
        self.assertEqual(lpi.load_cache(None, self.stamp, self.stats), {})


class GnStampTest(unittest.TestCase):
    def test_a_missing_binary_has_no_stamp(self):
        self.assertIsNone(lpi.gn_stamp("/nonexistent/gn"))

    def test_a_binary_is_its_path_mtime_and_size(self):
        with tempfile.NamedTemporaryFile() as f:
            st = os.stat(f.name)
            self.assertEqual(lpi.gn_stamp(f.name),
                             [os.path.realpath(f.name), st.st_mtime_ns, st.st_size])

    def test_a_bare_name_is_looked_up_in_the_path(self):
        self.assertIsNotNone(lpi.gn_stamp("python3"))


class FileStatsTest(unittest.TestCase):
    def test_missing_files_are_skipped(self):
        with tempfile.TemporaryDirectory() as root:
            open(os.path.join(root, "a"), "w").close()
            stats = lpi.file_stats(root, ["a", "b"])
            st = os.stat(os.path.join(root, "a"))
            self.assertEqual(stats, {"a": [st.st_mtime_ns, st.st_size]})


if __name__ == "__main__":
    unittest.main()
