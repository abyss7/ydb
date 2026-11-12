import ast
import contextlib
import io
import os
import tempfile
import types
import unittest
from unittest.mock import Mock, patch

import follow
from follow import Follower, Git, State, Stop


class FollowHistoryTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = os.path.join(self.tmp.name, "repo")
        os.mkdir(self.root)
        self.git = Git(self.root)
        self.git.out("init", "-q")
        self.git.out("config", "user.name", "Follow test")
        self.git.out("config", "user.email", "follow@example.invalid")
        self.git.out("config", "commit.gpgsign", "false")
        self.git.out("commit", "-q", "--allow-empty", "-m", "base")
        self.base = self.git.out("rev-parse", "HEAD")
        self.git.out("checkout", "-q", "-b", "upstream")
        with open(os.path.join(self.root, "source.cpp"), "w") as f:
            f.write("// upstream\n")
        self.git.out("add", "source.cpp")
        self.git.out("commit", "-q", "-m", "upstream change")
        self.u = self.git.out("rev-parse", "HEAD")
        self.git.out("checkout", "-q", "-b", "front", self.base)

    def commit(self, message):
        self.git.out("commit", "-q", "--allow-empty", "-m", message)
        return self.git.out("rev-parse", "HEAD")

    def merge(self, message=None):
        message = message or "Merge %s: upstream change" % self.u[:11]
        self.git.out("merge", "-q", "--no-ff", "-m", message, self.u)
        return self.git.out("rev-parse", "HEAD")

    def sync(self):
        return self.commit("gn: sync %s: upstream change" % self.u[:11])

    def follower(self):
        # All build/generator/bench stages are mocked; only a disposable Git
        # repository is used. The real follow/current/step/commit flow is kept.
        f = Follower.__new__(Follower)
        f.args = types.SimpleNamespace(upstream="upstream", fetch=False, skip_bench=False,
                                       max_commits=1, env=[], push=None, only_head=False)
        f.root, f.git = self.root, self.git
        f.env = dict(os.environ)
        f.state = State(os.path.join(self.tmp.name, "state"))
        f.say = Mock()
        f.regen = Mock()
        f.gn_gen = Mock()
        f.build = Mock()
        f.review = Mock()
        f.bench = Mock(return_value=[])
        f.bootstrap = Mock()
        return f

    def test_unmanaged_history(self):
        self.assertIsNone(self.git.follow_step())

    def test_pending_merge_below_manual_fix(self):
        self.merge()
        self.commit("fix follow script")
        self.assertEqual(self.git.follow_step(), (self.u, None))

    def test_sync_below_manual_fix(self):
        self.merge()
        sync = self.sync()
        self.commit("manual fix")
        self.assertEqual(self.git.follow_step(), (self.u, sync))

    def test_sync_before_merge_does_not_complete_it(self):
        self.sync()
        self.merge()
        self.assertEqual(self.git.follow_step(), (self.u, None))

    def test_unmanaged_merge_is_a_boundary(self):
        self.merge("manual merge")
        self.sync()
        self.assertIsNone(self.git.follow_step())

    def test_wrong_merge_sha_is_rejected(self):
        self.merge("Merge %s: wrong parent" % self.base[:11])
        with self.assertRaises(Stop):
            self.git.follow_step()

    def test_wrong_sync_sha_is_rejected(self):
        self.merge()
        self.commit("gn: sync %s: wrong upstream" % self.base[:11])
        with self.assertRaises(Stop):
            self.git.follow_step()

    def test_old_upstream_ref_does_not_hide_pending_step(self):
        self.merge()
        self.git.out("branch", "old-upstream", self.base)
        f = self.follower()
        f.args.upstream = "old-upstream"
        f.state.restore(self.git, f.args.upstream, persist=True)
        self.assertEqual(f.current(), (self.u, True))
        sync = self.sync()
        f.state.append({"status": "done", "upstream": self.u, "result": sync})
        self.assertIsNone(f.current())

    def test_diverged_upstream_is_rejected(self):
        self.merge()
        self.sync()
        self.git.out("checkout", "-q", "-b", "diverged", self.base)
        self.commit("other upstream")
        self.git.out("checkout", "-q", "front")
        f = self.follower()
        f.args.upstream = "diverged"
        with self.assertRaises(Stop):
            f.state.restore(self.git, f.args.upstream, persist=True)

    def test_fresh_pending_step_runs_locally_and_creates_empty_sync(self):
        merge = self.merge()
        self.commit("fix follow script")
        f = self.follower()
        # Also covers retrying after the old script's bootstrap STOP.
        f.state.append({"status": "stopped", "stage": "idempotence", "upstream": "bootstrap-old"})
        self.assertEqual(f.follow(), 0)
        f.bootstrap.assert_not_called()
        f.review.assert_called_once()
        self.assertFalse(f.regen.call_args.kwargs["idempotence"])
        f.gn_gen.assert_called_once()
        f.build.assert_called_once()
        self.assertTrue(f.state.bootstrapped())
        self.assertIn(self.u, f.state.done())
        self.assertTrue(self.git.subject("HEAD").startswith("gn: sync " + self.u[:11] + ":"))
        self.assertEqual(self.git.out("rev-parse", "HEAD^{tree}"),
                         self.git.out("rev-parse", merge + "^{tree}"))
        self.assertIsNone(f.current())
        self.assertEqual(f.follow(), 0)
        f.build.assert_called_once()

    def test_recovery_keeps_uncommitted_fixes(self):
        self.merge()
        with open(os.path.join(self.root, "source.cpp"), "a") as out:
            out.write("// local fix\n")
        f = self.follower()
        f.follow()
        self.assertIn("// local fix", self.git.out("show", "HEAD:source.cpp"))
        self.assertFalse(self.git.dirty())

    def test_docs_step_also_creates_empty_sync(self):
        merge = self.merge()
        f = self.follower()
        f.state.append({"status": "bootstrap", "upstream": self.base})
        with patch("follow.classify", return_value={"files": 1, "review": [], "info": [], "build": False}):
            f.follow()
        f.regen.assert_not_called()
        f.build.assert_not_called()
        self.assertNotEqual(self.git.out("rev-parse", "HEAD"), merge)
        self.assertIsNotNone(self.git.follow_step()[1])

    def test_fresh_docs_merge_still_builds_locally(self):
        self.merge()
        f = self.follower()
        with patch("follow.classify", return_value={"files": 1, "review": [], "info": [], "build": False}):
            f.follow()
        f.regen.assert_called_once()
        f.build.assert_called_once()

    def test_legacy_bench_stop_without_sync_preserves_local_progress(self):
        self.merge()
        f = self.follower()
        f.state.append({"status": "bootstrap", "upstream": self.base})
        f.state.append({"status": "stopped", "stage": "bench", "upstream": self.u})
        f.follow()
        f.regen.assert_not_called()
        f.bench.assert_called_once()
        self.assertIsNone(self.git.follow_step()[1])
        self.assertIsNone(f.current())

    def test_fresh_synced_step_still_bootstraps_locally(self):
        self.merge()
        self.sync()
        self.commit("fix follow script")
        f = self.follower()
        f.follow()
        f.bootstrap.assert_called_once_with()
        f.regen.assert_not_called()
        f.bench.assert_not_called()

    def test_existing_journal_is_not_reinterpreted_from_history(self):
        self.merge()
        f = self.follower()
        f.state.append({"status": "bootstrap", "upstream": self.base})
        f.state.append({"status": "done", "upstream": self.u})
        with patch.object(self.git, "follow_step", side_effect=AssertionError("unexpected history scan")):
            f.follow()
            self.assertIsNone(f.current())
        f.regen.assert_not_called()

    def test_bench_stop_resumes_without_another_sync(self):
        self.merge()
        sync = self.sync()
        f = self.follower()
        f.state.append({"status": "bootstrap", "upstream": self.base})
        f.state.append({"status": "stopped", "stage": "bench", "upstream": self.u})
        self.assertEqual(f.current(), (self.u, True))
        f.follow()
        f.regen.assert_not_called()
        f.bench.assert_called_once()
        self.assertEqual(self.git.out("rev-parse", "HEAD"), sync)
        self.assertIsNone(f.current())

    def test_bootstrap_does_not_create_an_empty_commit(self):
        self.merge()
        f = self.follower()
        before = self.git.out("rev-parse", "HEAD")
        f.commit("gn: regen at " + self.u[:11], {})
        self.assertEqual(self.git.out("rev-parse", "HEAD"), before)

    def test_restore_preview_does_not_write_journal(self):
        self.merge()
        f = self.follower()
        restored = f.state.restore(self.git, "upstream")
        self.assertEqual(restored["status"], "recovered")
        self.assertNotIn(self.u, f.state.done())
        self.assertFalse(os.path.exists(f.state.journal))
        self.assertEqual(f.current(), (self.u, True))
        self.assertEqual(State(f.state.path).records(), [])

    def test_restore_is_persisted_once_and_not_reparsed(self):
        self.merge()
        f = self.follower()
        with patch.object(self.git, "follow_step", wraps=self.git.follow_step) as scan:
            f.follow()
            f.follow()
        scan.assert_called_once()
        records = State(f.state.path).records()
        self.assertEqual(sum(bool(r.get("from_history")) for r in records), 1)
        self.assertIn(self.u, State(f.state.path).done())

    def test_restored_sync_requires_real_local_bootstrap(self):
        self.merge()
        sync = self.sync()
        f = self.follower()
        f.bootstrap = types.MethodType(Follower.bootstrap, f)
        f.follow()
        self.assertTrue(f.regen.call_args.kwargs["idempotence"])
        f.build.assert_called_once()
        self.assertTrue(f.bench.call_args.kwargs["full"])
        self.assertEqual(self.git.out("rev-parse", "HEAD"), sync)
        self.assertTrue(f.state.bootstrapped())

    def test_bootstrap_stop_after_restore_is_not_overwritten(self):
        self.merge()
        self.sync()
        f = self.follower()
        f.state.restore(self.git, "upstream", persist=True)
        stop = {"status": "stopped", "stage": "build", "upstream": "bootstrap-" + self.u[:11]}
        f.state.append(stop)
        with patch.object(self.git, "follow_step", side_effect=AssertionError("unexpected history scan")):
            self.assertIsNone(f.state.restore(self.git, "upstream", persist=True))
        self.assertEqual(f.state.last_stop()["stage"], "build")

    def test_restored_anchor_does_not_hide_a_newer_merge(self):
        self.merge()
        f = self.follower()
        f.state.restore(self.git, "upstream", persist=True)
        self.sync()
        f.state.append({"status": "done", "upstream": self.u})
        self.git.out("checkout", "-q", "upstream")
        next_u = self.commit("next upstream")
        self.git.out("checkout", "-q", "front")
        self.git.out("merge", "-q", "--no-ff", "-m", "Merge %s: next upstream" % next_u[:11], next_u)
        self.assertEqual(f.current(), (next_u, True))


    def test_state_name_is_per_branch(self):
        self.assertEqual(follow.state_name("origin/main"), "main")
        self.assertEqual(follow.state_name("origin/stable-25.1"), "stable-25.1")
        self.assertEqual(follow.state_name("origin/ydb/stable-25.1"), "ydb-stable-25.1")
        self.assertEqual(follow.state_name("upstream"), "upstream")

    def test_missing_ref_is_a_usage_error(self):
        ap = Mock()
        ap.error.side_effect = SystemExit(2)
        with self.assertRaises(SystemExit):
            follow.require_ref(ap, self.root, "no-such-ref")
        follow.require_ref(ap, self.root, "upstream")

    def test_only_head_merges_the_head_once(self):
        self.git.out("checkout", "-q", "upstream")
        with open(os.path.join(self.root, "second.cpp"), "w") as out:
            out.write("// second upstream\n")
        self.git.out("add", "second.cpp")
        self.git.out("commit", "-q", "-m", "second upstream change")
        tip = self.git.out("rev-parse", "HEAD")
        self.git.out("checkout", "-q", "front")
        f = self.follower()
        f.args.only_head, f.args.max_commits = True, None
        f.state.append({"status": "bootstrap", "upstream": self.base})
        with patch("follow.classify", wraps=follow.classify) as classify:
            self.assertEqual(f.follow(), 0)
        classify.assert_called_once()
        self.assertEqual(classify.call_args.args[4], self.base)     # the whole range, not U^1
        f.regen.assert_called_once()
        self.assertEqual(self.git.out("rev-list", "--count", "--merges", "HEAD"), "1")
        merge = self.git.out("rev-parse", "HEAD^")
        self.assertEqual(self.git.out("rev-parse", merge + "^2"), tip)
        self.assertTrue(self.git.subject("HEAD").startswith("gn: sync " + tip[:11] + ":"))
        rec = [r for r in f.state.records() if r["upstream"] == tip and r["status"] == "done"][0]
        self.assertEqual((rec["mode"], rec["skipped"]), ("only-head", 2))
        self.assertIsNone(f.current())


class ConfigTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.ap = Mock()
        self.ap.error.side_effect = SystemExit(2)

    def args(self, **kw):
        """The arguments as argparse leaves them (every one None unless given)."""
        fields = dict({k: None for k in follow.CONFIG_DEFAULTS},
                      run=False, only_head=False, skip_bench=False, bench=False, fetch=False,
                      max_commits=None, branch=None, upstream=None)
        fields.update(kw)
        return types.SimpleNamespace(**fields)

    def test_branch_gives_the_upstream_ref(self):
        self.assertEqual(follow.resolve_upstream(self.ap, self.args()), "origin/main")
        self.assertEqual(follow.resolve_upstream(self.ap, self.args(branch="stable-25.1")),
                         "origin/stable-25.1")
        self.assertEqual(follow.resolve_upstream(self.ap, self.args(upstream="other/main")), "other/main")
        with self.assertRaises(SystemExit):
            follow.resolve_upstream(self.ap, self.args(branch="main", upstream="other/main"))

    def test_only_head_rejects_max_commits(self):
        with self.assertRaises(SystemExit):
            follow.validate(self.ap, self.args(only_head=True, max_commits=3))
        args = self.args(max_commits="all")
        follow.validate(self.ap, args)
        self.assertIsNone(args.max_commits)

    def test_given_arguments_are_remembered(self):
        path = os.path.join(self.tmp.name, "state", "config.json")
        cfg, given = follow.load_config(self.args(bench_root=self.tmp.name), path)
        self.assertEqual(sorted(given), ["bench_root"])
        with contextlib.redirect_stdout(io.StringIO()):
            follow.save_config(path, cfg, given)
        args = self.args()
        follow.load_config(args, path)
        self.assertEqual(args.bench_root, os.path.realpath(self.tmp.name))
        self.assertEqual(args.nice, follow.CONFIG_DEFAULTS["nice"])
        self.assertEqual(args.out, None)        # <root>/out/gn is resolved after the config


class LinkPoolArgsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = os.path.join(self.tmp.name, "out", "gn", "args.gn")

    def test_creates_the_file_with_the_arg(self):
        self.assertTrue(follow.set_link_pool_size(self.path, 34))
        self.assertEqual(follow.read_text(self.path), "link_pool_size = 34\n")

    def test_replaces_the_arg_and_keeps_the_rest(self):
        follow.write_text(self.path, 'pch_mode = "global"\nlink_pool_size = 8\n')
        self.assertTrue(follow.set_link_pool_size(self.path, 17))
        self.assertEqual(follow.read_text(self.path), 'pch_mode = "global"\nlink_pool_size = 17\n')

    def test_the_same_value_writes_nothing(self):
        follow.set_link_pool_size(self.path, 17)
        self.assertFalse(follow.set_link_pool_size(self.path, 17))


class NeedGenTest(unittest.TestCase):
    """gn_gen() runs only when `gn gen` has something new to read."""

    INPUTS = ["../../a/BUILD.gn", "./args.gn"]

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.out = os.path.join(self.tmp.name, "out", "gn")
        self.git = Mock()
        self.git.added_paths.return_value = []

    def need_gen(self, **rec):
        return Follower.need_gen(types.SimpleNamespace(out=self.out, git=self.git), rec)

    def generated(self, stamp=10 ** 18, newer=(), missing=()):
        """A build dir as the last `gn gen` left it: its inputs older than its
        build.ninja, `newer` ones written after, `missing` ones gone."""
        follow.write_text(os.path.join(self.out, "build.ninja.d"),
                          "build.ninja.stamp: " + " ".join(self.INPUTS) + "\n")
        follow.write_text(os.path.join(self.out, "build.ninja"), "")
        os.utime(os.path.join(self.out, "build.ninja"), ns=(stamp, stamp))
        for path in self.INPUTS:
            if path in missing:
                continue
            full = os.path.join(self.out, path)
            follow.write_text(full, "")
            ns = stamp + 100 if path in newer else stamp
            os.utime(full, ns=(ns, ns))

    def test_no_build_dir_means_gen(self):
        self.assertTrue(self.need_gen())

    def test_no_depfile_means_gen(self):
        follow.write_text(os.path.join(self.out, "build.ninja"), "")
        self.assertTrue(self.need_gen())

    def test_unchanged_inputs_skip_the_gen(self):
        self.generated()
        self.assertFalse(self.need_gen())

    def test_an_input_written_after_the_gen_forces_it(self):
        self.generated(newer=("../../a/BUILD.gn",))
        self.assertTrue(self.need_gen())

    def test_an_input_that_disappeared_forces_it(self):
        self.generated(missing=("../../a/BUILD.gn",))
        self.assertTrue(self.need_gen())

    def test_new_link_pool_size_forces_it(self):
        self.generated()
        self.assertTrue(self.need_gen(args_gn_changed=True))

    def test_a_new_build_file_forces_it(self):
        self.generated()
        self.git.added_paths.return_value = ["ydb/core/new/BUILD.gn"]
        self.assertTrue(self.need_gen())

    def test_a_new_gni_forces_it(self):
        self.generated()
        self.git.added_paths.return_value = ["ydb/core/new/ya.gni"]
        self.assertTrue(self.need_gen())

    def test_a_new_source_does_not(self):
        self.generated()
        self.git.added_paths.return_value = ["ydb/core/health_check/health_check.cpp",
                                             "ydb/core/health_check/ya.make"]
        self.assertFalse(self.need_gen())

    def test_depfile_continuations_are_read(self):
        follow.write_text(os.path.join(self.out, "build.ninja.d"),
                          "build.ninja.stamp: ../../a/BUILD.gn \\\n ./args.gn\n")
        follower = types.SimpleNamespace(out=self.out)
        self.assertEqual(follow.gen_inputs(self.out), ["../../a/BUILD.gn", "./args.gn"])

    def test_a_source_is_not_a_gen_input(self):
        self.assertTrue(follow.gen_input("x/BUILD.gn"))
        self.assertFalse(follow.gen_input("x/ya.make"))
        self.assertFalse(follow.gen_input("x/health_check_ut.cpp"))


def percent_messages(path):
    """(line, text, value expression) of every `%`-formatted string literal."""
    for node in ast.walk(ast.parse(open(path).read())):
        if not (isinstance(node, ast.BinOp) and isinstance(node.op, ast.Mod)):
            continue
        left, parts = node.left, []
        while isinstance(left, ast.BinOp) and isinstance(left.op, ast.Add) and isinstance(left.right, ast.Constant):
            parts.insert(0, left.right.value)
            left = left.left
        if isinstance(left, ast.Constant) and isinstance(left.value, str):
            parts.insert(0, left.value)
            yield node.lineno, "".join(parts), node.right


def percent_specs(text):
    """How many values the string wants."""
    specs, i = 0, 0
    while i < len(text) - 1:
        if text[i] == "%":
            if text[i + 1] == "%":
                i += 2
                continue
            specs += 1
            while i + 1 < len(text) and text[i + 1] not in "sdrfx":
                i += 1
        i += 1
    return specs


class MessageFormatTest(unittest.TestCase):
    """A STOP message is the explanation of the stop: a `%` with the wrong
    number of values raises inside the raise and hides it (the review STOP did,
    2026-11-12: the `30` limit went into the tuple of values, not into the
    listing() call)."""

    def test_every_message_has_its_values(self):
        path = os.path.join(os.path.dirname(os.path.abspath(follow.__file__)), "follow.py")
        bad = []
        for line, text, value in percent_messages(path):
            specs = percent_specs(text)
            if isinstance(value, ast.Tuple):
                values = len(value.elts)
            elif specs == 1:
                values = 1
            else:
                continue        # a named value: its arity is not visible here
            if specs != values:
                bad.append("%s:%d: %d specs, %d values" % (os.path.basename(path), line, specs, values))
        self.assertEqual(bad, [])


if __name__ == "__main__":
    unittest.main()
