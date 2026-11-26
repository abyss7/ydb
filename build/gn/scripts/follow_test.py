import ast
import contextlib
import io
import os
import sys
import tempfile
import types
import unittest
from unittest.mock import Mock, patch

import follow
import follow_bench
from follow import Follower, FollowWork, Git, State, Stop


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

    def follower(self, migrate=False, **settings):
        # All build/generator/bench stages are mocked; only a disposable Git
        # repository is used. The real follow/current/step/commit flow is kept.
        f = Follower.__new__(Follower)
        options = dict(upstream="upstream", fetch=False, skip_bench=False, bench=False,
                       max_commits=1, env=[], push=None, only_head=False, run=True,
                       branch=None, state=os.path.join(self.tmp.name, "state"))
        f.args = types.SimpleNamespace(**dict(options, **settings))
        f.root, f.git = self.root, self.git
        f.env = dict(os.environ)
        f.state = State(f.args.state)
        f.work = FollowWork(self.git, os.path.join(self.root, ".git", "gn-follow", "work.json"))
        if migrate:
            old = self.git.ref("HEAD")
            target, work_ref = f.work.target, f.work.ref
            self.git.out("update-ref", work_ref, old)
            self.git.out("symbolic-ref", "HEAD", work_ref)
            self.git.out("update-ref", target, self.base, old)
            f.work.data = dict(target=target, base=self.base, upstream=f.args.upstream, state=f.args.state)
            f.work.save()
        f.work.start(f.args)
        f.base, f.published_base, f.work_ref = f.work.gn_base, f.work.data["base"], f.work.ref
        f.say = Mock()
        f.regen = Mock()
        f.gn_gen = Mock()
        f.build = Mock()
        f.review = Mock()
        f.bench = Mock(return_value=[])
        f.bootstrap = Mock()
        return f

    def add_upstream(self, path):
        head = self.git.head_ref()[len("refs/heads/"):]
        self.git.out("switch", "-q", "upstream")
        follow.write_text(os.path.join(self.root, path), path + "\n")
        self.git.out("add", "--", path)
        tip = self.commit("Add " + path)
        self.git.out("switch", "-q", head)
        return tip

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
        f = self.follower(migrate=True, upstream="old-upstream")
        f.state.restore(self.git, f.args.upstream, persist=True)
        self.assertEqual(f.current(), (self.u, True))
        f.step(self.u, True)
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

    def test_migrated_merge_is_verified_without_publishing_the_unchecked_commit(self):
        merge = self.merge()
        self.commit("fix follow script")
        f = self.follower(migrate=True)
        # Also covers retrying after the old script's bootstrap STOP.
        f.state.append({"status": "stopped", "stage": "idempotence", "upstream": "bootstrap-old"})
        self.assertEqual(f.follow(), self.git.ref("HEAD"))
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
        self.assertEqual(self.git.out("show", "-s", "--format=%P", "HEAD").split(), [self.base, self.u])
        self.assertFalse(self.git.ancestor(merge, "HEAD"))
        self.assertEqual(self.git.ref("front"), self.git.ref("HEAD"))
        self.assertIsNone(f.current())
        f.follow()
        f.build.assert_called_once()

    def test_recovery_keeps_uncommitted_fixes(self):
        self.merge()
        with open(os.path.join(self.root, "source.cpp"), "a") as out:
            out.write("// local fix\n")
        f = self.follower(migrate=True)
        f.follow()
        self.assertIn("// local fix", self.git.out("show", "HEAD:source.cpp"))
        self.assertFalse(self.git.dirty())

    def test_docs_only_change_does_not_rebuild_verified_base(self):
        self.git.out("branch", "-f", "upstream", self.base)
        docs = self.add_upstream(".github/example.md")
        f = self.follower()
        f.state.append({"status": "bootstrap", "upstream": self.base})
        result = f.follow()
        f.regen.assert_not_called()
        f.build.assert_not_called()
        self.assertEqual(self.git.follow_step(), (docs, result))

    def test_fresh_docs_merge_still_builds_locally(self):
        self.merge()
        f = self.follower(migrate=True)
        with patch("follow.classify", return_value={"files": 1, "review": [], "info": [], "build": False}):
            f.follow()
        f.regen.assert_called_once()
        f.build.assert_called_once()

    def test_legacy_bench_stop_without_verified_sha_rebuilds(self):
        old = self.merge()
        f = self.follower(migrate=True, bench=True)
        f.state.append({"status": "bootstrap", "upstream": self.base})
        f.state.append({"status": "stopped", "stage": "bench", "upstream": self.u})
        f.follow()
        f.regen.assert_called_once()
        f.build.assert_called_once()
        f.bench.assert_called_once()
        self.assertFalse(self.git.ancestor(old, "HEAD"))
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
        sync = self.sync()
        f = self.follower()
        f.state.append({"status": "bootstrap", "upstream": self.base})
        f.state.append({"status": "done", "upstream": self.u, "result": sync})
        with patch.object(self.git, "follow_step", side_effect=AssertionError("unexpected history scan")):
            f.follow()
            self.assertIsNone(f.current())
        f.regen.assert_not_called()

    def test_bench_stop_resumes_without_another_sync(self):
        f = self.follower(bench=True)
        f.state.append({"status": "bootstrap", "upstream": self.base})
        f.bench.side_effect = Stop("bench", "measuring failed")
        with self.assertRaises(Stop):
            f.follow()
        sync = self.git.ref("HEAD")
        self.assertEqual(f.state.last_stop()["result"], sync)
        self.assertEqual(self.git.ref("front"), self.base)
        self.assertEqual(f.current(), (self.u, True))
        f.bench.reset_mock(side_effect=True)
        f.bench.return_value = ["GN-Bench-Ninja: sample"]
        result = f.follow()
        f.regen.assert_called_once()
        f.build.assert_called_once()
        f.bench.assert_called_once()
        self.assertEqual(self.git.out("rev-parse", result + "^{tree}"), self.git.out("rev-parse", sync + "^{tree}"))
        self.assertIn("GN-Bench-Ninja: sample", self.git.out("show", "-s", "--format=%B", result))
        self.assertFalse(self.git.ancestor(sync, result))
        self.assertIsNone(f.current())

    def test_bootstrap_does_not_create_an_empty_commit(self):
        f = self.follower()
        before = self.git.out("rev-parse", "HEAD")
        f.commit("gn: regen at " + self.u[:11], {}, (before,))
        self.assertEqual(self.git.out("rev-parse", "HEAD"), before)

    def test_restore_preview_does_not_write_journal(self):
        self.merge()
        f = self.follower(migrate=True)
        restored = f.state.restore(self.git, "upstream")
        self.assertEqual(restored["status"], "recovered")
        self.assertNotIn(self.u, f.state.done())
        self.assertFalse(os.path.exists(f.state.journal))
        self.assertEqual(f.current(), (self.u, True))
        self.assertEqual(State(f.state.path).records(), [])

    def test_restore_is_persisted_once_and_not_reparsed(self):
        self.merge()
        self.sync()
        f = self.follower()
        f.bootstrap = types.MethodType(Follower.bootstrap, f)
        with patch.object(self.git, "follow_step", wraps=self.git.follow_step) as scan:
            f.follow()
            f.follow()
        self.assertEqual(sum(call.args == (f.base,) for call in scan.call_args_list), 1)
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
        self.sync()
        f = self.follower()
        f.state.restore(self.git, "upstream", persist=True)
        next_u = self.add_upstream("next.cpp")
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
        f = self.follower(only_head=True, max_commits=None)
        f.state.append({"status": "bootstrap", "upstream": self.base})
        with patch("follow.classify", wraps=follow.classify) as classify:
            result = f.follow()
        classify.assert_called_once()
        self.assertEqual(classify.call_args.args[4], self.base)     # the whole range, not U^1
        f.regen.assert_called_once()
        self.assertEqual(self.git.out("rev-list", "--count", "--merges", "HEAD"), "1")
        self.assertEqual(self.git.out("rev-parse", result + "^2"), tip)
        self.assertTrue(self.git.subject("HEAD").startswith("gn: sync " + tip[:11] + ":"))
        rec = [r for r in f.state.records() if r["upstream"] == tip and r["status"] == "done"][0]
        self.assertEqual((rec["mode"], rec["skipped"]), ("only-head", 2))
        self.assertIsNone(f.current())

    def test_failed_later_step_keeps_checkpoint_and_resume_keeps_batch_limit(self):
        second = self.add_upstream("second.cpp")
        self.add_upstream("third.cpp")
        f = self.follower(max_commits=2)
        f.state.append({"status": "bootstrap", "upstream": self.base})
        f.build.side_effect = [None, Stop("build", "failed")]
        with self.assertRaisesRegex(Stop, "failed"):
            f.follow()
        unchecked = self.git.ref("HEAD")
        self.assertEqual(self.git.ref("front"), self.git.ref(unchecked + "^1"))
        self.assertEqual(self.git.ref("front^2"), self.u)
        self.assertEqual(self.git.head_ref(), f.work_ref)
        f.work = FollowWork(self.git, f.work.path)
        f.args.max_commits = None
        follow.load_run_settings(Mock(), f.args, self.git, os.path.join(self.root, ".git"), f.work)
        self.assertEqual(f.args.max_commits, 2)
        f.build.reset_mock(side_effect=True)
        result = f.follow()
        f.build.assert_called_once()
        self.assertEqual(self.git.ref(result + "^2"), second)
        self.assertFalse(self.git.ancestor(unchecked, result))
        self.assertEqual(self.git.ref("front"), result)
        source = os.path.join(self.root, "source.cpp")
        before = os.stat(source).st_mtime_ns
        f.work.publish(result)
        self.assertEqual(os.stat(source).st_mtime_ns, before)
        self.assertEqual(self.git.ref("front"), result)
        self.assertEqual(self.git.head_ref(), "refs/heads/front")
        self.assertFalse(self.git.ref(f.work_ref))
        self.assertFalse(os.path.exists(f.work.path))

    def test_conflict_keeps_index_and_resumes_on_work_branch(self):
        source = os.path.join(self.root, "source.cpp")
        follow.write_text(source, "// GN\n")
        self.git.out("add", "--", "source.cpp")
        base = self.commit("GN version")
        f = self.follower()
        f.state.append({"status": "bootstrap", "upstream": self.base})
        with self.assertRaisesRegex(Stop, "left conflicts"):
            f.follow()
        self.assertEqual(self.git.ref("front"), base)
        self.assertEqual(self.git.merge_head(), self.u)
        self.assertEqual(self.git.unmerged(), ["source.cpp"])
        follow.write_text(source, "// resolved\n")
        self.git.out("add", "--", "source.cpp")
        f.work.start(f.args)
        result = f.follow()
        self.assertEqual(self.git.out("show", result + ":source.cpp"), "// resolved")
        self.assertEqual(self.git.ref("front"), result)

    def test_interrupt_leaves_work_and_does_not_move_target(self):
        f = self.follower()
        f.state.append({"status": "bootstrap", "upstream": self.base})
        f.build.side_effect = KeyboardInterrupt
        with self.assertRaises(KeyboardInterrupt):
            f.follow()
        self.assertEqual(self.git.ref("front"), self.base)
        self.assertEqual(self.git.head_ref(), f.work_ref)
        self.assertTrue(os.path.exists(f.work.path))
        f.build.reset_mock(side_effect=True)
        result = f.follow()
        f.work.publish(result)
        self.assertEqual(self.git.ref("front"), result)

    def test_publish_does_not_overwrite_a_changed_target(self):
        f = self.follower()
        f.state.append({"status": "bootstrap", "upstream": self.base})

        def change_target(*args):
            self.git.out("update-ref", "refs/heads/front", self.u, self.base)

        f.build.side_effect = change_target
        with self.assertRaisesRegex(Stop, "changed since"):
            f.follow()
        self.assertEqual(self.git.ref("front"), self.u)
        self.assertEqual(self.git.ref(f.work_ref + "^2"), self.u)

    def test_push_targets_only_the_published_branch(self):
        remote = os.path.join(self.tmp.name, "remote.git")
        self.git.out("init", "-q", "--bare", remote)
        f = self.follower()
        f.state.append({"status": "bootstrap", "upstream": self.base})
        result = f.follow()
        f.work.publish(result, remote)
        refs = Git(remote).lines("for-each-ref", "--format=%(refname) %(objectname)", "refs/heads")
        self.assertEqual(refs, ["refs/heads/front " + result])

    def test_unregistered_unfinished_merge_is_not_used_as_published_base(self):
        merge = self.merge()
        with self.assertRaisesRegex(Stop, "unfinished merge"):
            self.follower()
        self.assertEqual(self.git.ref("front"), merge)

    def track_gn(self, tip):
        self.git.out("branch", "infra", tip)
        self.git.out("branch", "--set-upstream-to=infra", "front")

    def infrastructure_update(self):
        self.git.out("switch", "-q", "-c", "gn-infra", self.base)
        follow.write_text(os.path.join(self.root, "tool.py"), "updated infrastructure\n")
        self.git.out("add", "--", "tool.py")
        infra = self.commit("Update build infrastructure")
        self.git.out("switch", "-q", "front")
        return infra

    def test_gn_tracking_update_without_source_updates_is_checked_before_publication(self):
        self.git.out("branch", "-f", "upstream", self.base)
        infra = self.infrastructure_update()
        self.track_gn(infra)
        f = self.follower()
        self.assertEqual(f.base, infra)
        self.assertEqual(self.git.ref("HEAD"), infra)
        self.assertEqual(self.git.ref("front"), self.base)
        f.state.append({"status": "bootstrap", "upstream": self.base, "result": self.base})
        f.bootstrap = types.MethodType(Follower.bootstrap, f)
        f.build.side_effect = Stop("build", "infrastructure build failed")
        with self.assertRaisesRegex(Stop, "infrastructure build failed"):
            f.follow()
        self.assertEqual(self.git.ref("front"), self.base)
        f.build.reset_mock(side_effect=True)
        result = f.follow()
        f.build.assert_called_once()
        self.assertEqual(result, infra)
        f.work.publish(result)
        self.assertEqual(self.git.ref("front"), infra)

    def test_migrated_step_keeps_infrastructure_ancestry_without_raw_merge(self):
        infra = self.infrastructure_update()
        dirty_merge = self.merge()
        f = self.follower(migrate=True)
        f.work.data["gn_base"] = f.base = infra
        f.work.save()
        follow.write_text(os.path.join(self.root, "tool.py"), "updated infrastructure\n")
        result = f.follow()
        f.bootstrap.assert_not_called()
        f.build.assert_called_once()
        self.assertEqual(self.git.ref(result + "^1"), infra)
        self.assertEqual(self.git.ref(result + "^2"), self.u)
        self.assertEqual(self.git.out("show", result + ":tool.py"), "updated infrastructure")
        self.assertFalse(self.git.ancestor(dirty_merge, result))
        self.assertEqual(self.git.ref("front"), result)
        f.work.publish(result)
        self.assertEqual(self.git.ref("front"), result)

    def test_rebases_published_sync_onto_tracking_with_source_parent(self):
        f = self.follower()
        f.state.append({"status": "bootstrap", "upstream": self.base})
        f.step(self.u, False)  # a verified but unpublished step from the old script
        local = self.git.ref("HEAD")
        infra = self.infrastructure_update()
        self.track_gn(infra)
        self.git.out("switch", "-q", f.work_ref[len("refs/heads/"):])
        f.build.reset_mock()
        result = f.follow()
        self.assertEqual(self.git.ref("front"), result)
        self.assertEqual(self.git.ref(result + "^1"), infra)
        self.assertEqual(self.git.ref(result + "^2"), self.u)
        self.assertFalse(self.git.ancestor(local, result))
        f.build.assert_not_called()

    def test_retroactive_publication_leaves_unfinished_step_alone(self):
        second = self.add_upstream("second.cpp")
        f = self.follower(max_commits=2)
        f.state.append({"status": "bootstrap", "upstream": self.base})
        f.step(self.u, False)
        local = self.git.ref("HEAD")
        f.merge(second, {})
        pending = self.git.ref("HEAD")
        path = os.path.join(self.root, "second.cpp")
        follow.write_text(path, "// keep this fix\n")
        f.build.side_effect = Stop("build", "unfinished")
        with self.assertRaisesRegex(Stop, "unfinished"):
            f.follow()
        self.assertEqual(self.git.ref("front"), local)
        self.assertEqual(self.git.ref("HEAD"), pending)
        self.assertEqual(follow.read_text(path), "// keep this fix\n")

    def test_tracking_conflict_warns_and_keeps_latest_checkpoint(self):
        infra = self.infrastructure_update()
        self.track_gn(self.base)
        f = self.follower()
        f.state.append({"status": "bootstrap", "upstream": self.base})
        path = os.path.join(self.root, "tool.py")
        f.regen.side_effect = lambda *args, **kw: follow.write_text(path, "local infrastructure\n")

        def fetch(args):
            self.assertEqual(self.git.ref("front"), self.git.ref("HEAD"))
            self.git.out("update-ref", infra_ref, infra, self.base)

        infra_ref = "refs/heads/infra"
        with patch.object(f.work, "fetch", side_effect=fetch):
            result = f.follow()
        self.assertEqual(self.git.ref("front"), result)
        self.assertEqual(self.git.ref(result + "^2"), self.u)
        self.assertFalse(self.git.ancestor(infra, result))
        self.assertEqual(follow.read_text(path), "local infrastructure\n")
        self.assertFalse(self.git.operations())
        self.assertTrue(any("was not applied" in args[0] for args, _ in f.say.call_args_list))


class TargetTest(unittest.TestCase):
    def test_root_targets_do_not_select_ninja_all(self):
        replies = [types.SimpleNamespace(returncode=0, stdout=x + "\n", stderr="")
                   for x in ("//:all", "//:tests")]
        with patch.object(follow.subprocess, "run", side_effect=replies) as run:
            labels = follow.gn_targets("/src", "/out", follow.DEFAULT_TARGETS)
        self.assertEqual(follow.ninja_targets(labels), [":all", ":tests"])
        self.assertEqual([c.args[0][3] for c in run.call_args_list], ["//:all", "//:tests"])

    def test_bare_names_are_passed_to_gn_unchanged(self):
        replies = [types.SimpleNamespace(returncode=0, stdout=x + "\n", stderr="")
                   for x in ("//all:all", "//tests:tests")]
        with patch.object(follow.subprocess, "run", side_effect=replies) as run:
            labels = follow.gn_targets("/src", "/out", ["all", "tests"])
        self.assertEqual([c.args[0][3] for c in run.call_args_list], ["all", "tests"])
        self.assertEqual(follow.ninja_targets(labels), ["all:all", "tests:tests"])

    def test_directory_targets_are_resolved_by_gn(self):
        reply = types.SimpleNamespace(returncode=0, stdout="//ydb/apps/ydbd:ydbd\n", stderr="")
        with patch.object(follow.subprocess, "run", return_value=reply) as run:
            labels = follow.gn_targets("/src", "/out", ["ydb/apps/ydbd"], {"X": "1"}, "python3.9")
        self.assertEqual(follow.ninja_targets(labels), ["ydb/apps/ydbd:ydbd"])
        self.assertEqual(run.call_args.args[0],
                         ["gn", "ls", "/out", "ydb/apps/ydbd", "--as=label", "--default-toolchain",
                          "--script-executable=python3.9"])
        self.assertEqual(run.call_args.kwargs["cwd"], "/src")
        self.assertEqual(run.call_args.kwargs["env"], {"X": "1"})

    def test_missing_ambiguous_and_nondefault_targets_are_rejected(self):
        for output in ("", "//a:a\n//b:b\n", "//a:b(//toolchain:host)\n"):
            reply = types.SimpleNamespace(returncode=0, stdout=output, stderr="")
            with self.subTest(output=output), patch.object(follow.subprocess, "run", return_value=reply):
                with self.assertRaises(ValueError):
                    follow.gn_targets("/src", "/out", ["query"])

    def test_gn_errors_are_reported_without_fallback(self):
        reply = types.SimpleNamespace(returncode=1, stdout="ERROR Dependency cycle", stderr="")
        with patch.object(follow.subprocess, "run", return_value=reply):
            with self.assertRaisesRegex(ValueError, "Dependency cycle"):
                follow.gn_targets("/src", "/out", ["all"])

    def test_build_passes_explicit_alias_to_autofix(self):
        with tempfile.TemporaryDirectory() as root:
            f = object.__new__(Follower)
            f.root, f.out = root, os.path.join(root, "out")
            f.args = types.SimpleNamespace(python=sys.executable, jobs=None, targets=["all"],
                                          script_executable="python3.9")
            f.env = {}
            f.state = types.SimpleNamespace(blocked=os.path.join(root, "blocked"))
            f.progress = types.SimpleNamespace(stage=lambda *a, **kw: contextlib.nullcontext())
            f.say, f.last_seconds, f.sh = Mock(), Mock(return_value=0), Mock(return_value=(0, 0))
            with patch.object(follow, "gn_targets", return_value=["//:all"]) as resolve:
                f.build(root, {"seconds": {}})
            resolve.assert_called_once_with(root, f.out, ["all"], {}, "python3.9")
            self.assertEqual(f.sh.call_args.args[0][-1], ":all")

    def test_bench_expands_normalized_root_groups_for_ya(self):
        with tempfile.TemporaryDirectory() as root:
            follow.write_text(os.path.join(root, "BUILD.gn"),
                              'group("all") {\n  deps = [\n    "//app:main",\n  ]\n}\n')
            self.assertEqual(follow_bench.target_labels(root, ["//:all"]), ["//app:main"])
            self.assertEqual(follow_bench.target_labels(root, ["//other:all"]), ["//other:all"])


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
        args = self.args(max_commits="all", python=sys.executable,
                         script_executable=sys.executable)   # as after load_config
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

    def test_link_jobs_is_remembered_apart_from_jobs(self):
        path = os.path.join(self.tmp.name, "config.json")
        args = self.args(link_jobs=6)
        cfg, given = follow.load_config(args, path)
        self.assertEqual(given, {"link_jobs": 6})
        with contextlib.redirect_stdout(io.StringIO()):
            follow.save_config(path, cfg, given)
        args = self.args(jobs=12)               # --jobs stays the ninja one
        follow.load_config(args, path)
        self.assertEqual(args.link_jobs, 6)     # the remembered pool wins
        self.assertEqual(args.jobs, 12)
        args = self.args()                      # both fresh: the pool is --jobs'
        follow.load_config(args, os.path.join(self.tmp.name, "none.json"))
        self.assertIsNone(args.link_jobs)

    def test_the_pythons_default_and_are_remembered(self):
        path = os.path.join(self.tmp.name, "config.json")
        args = self.args()
        follow.load_config(args, path)
        self.assertEqual(args.python, follow.CONFIG_DEFAULTS["python"])    # the python of the run
        self.assertEqual(args.script_executable,
                         os.environ.get("GN_SCRIPT_EXECUTABLE", args.python))
        args = self.args(script_executable=sys.executable)
        cfg, given = follow.load_config(args, path)
        self.assertEqual(given, {"script_executable": sys.executable})
        with contextlib.redirect_stdout(io.StringIO()):
            follow.save_config(path, cfg, given)
        follow.load_config(self.args(), path)   # not given anymore: the remembered one
        args = self.args()
        follow.load_config(args, path)
        self.assertEqual(args.script_executable, sys.executable)


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
