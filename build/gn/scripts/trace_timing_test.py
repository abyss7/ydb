"""GN timing regression/parity tests. TRACE_TIMING_PROBE enables native checks."""

import array
import copy
import gzip
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

import build_stats
import build_time_report as report
import compile_time_traces
import pch_update
from trace_timing import normalized_events, trace_timing


PROBE = os.environ.get("TRACE_TIMING_PROBE")


def fixture():
    return {
        "traceEvents": [
            {"ph": "X", "name": "ExecuteCompiler", "ts": 10, "dur": 1000},
            {"ph": "b", "name": "Source", "ts": 100, "args": {"detail": "outer.h"}},
            {"ph": "X", "name": "Source", "ts": 200, "dur": 100, "args": {"detail": "inner.h"}},
            {"ph": "X", "name": "InstantiateClass", "ts": 300, "dur": 100, "args": {"detail": "Vec<int>"}},
            {"ph": "X", "name": "OptFunction", "ts": 400, "dur": 50, "args": {"detail": "function"}},
            {"ph": "e", "name": "Source", "ts": 700},
            {"ph": "X", "name": "Source", "ts": 800, "dur": 100, "args": {"detail": "other.h"}},
            *[{"ph": "X", "name": "Total " + name, "ts": 0, "dur": duration,
               "args": {"count": 3}} for name, duration in
              (("ExecuteCompiler", 1000), ("Frontend", 700), ("Backend", 250),
               ("Source", 700), ("InstantiateClass", 100), ("OptFunction", 50))],
        ],
        "beginningOfTime": 123456789,
        "distclang": {"version": 1, "execution_wall_us": 1000, "cpu_user_us": 500,
                      "cpu_system_us": 100, "queue_wait_us": 9000},
    }


class TimingTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="trace-timing-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.path = self.root / "a.json"
        self.paths = report.Paths(str(self.root), str(self.root), None)
        pch_update._init_trace_worker(self.paths, {})

    def write(self, data):
        self.path.write_text(json.dumps(data))
        return str(self.path)

    def summary(self, path):
        _, summary, error = pch_update.summarize_trace(("a.o", path))
        self.assertIsNone(error)
        return summary

    def parity(self, path):
        py = self.summary(path)
        native = json.loads(subprocess.check_output([PROBE, path], text=True))
        self.assertEqual(native["headers"], {
            h: [py["parse"][i], py["own"][i]] for i, h in enumerate(py["names"])})
        self.assertEqual(sorted(native["edges"]),
                         sorted([[py["names"][a], py["names"][b]] for a, b in py["edges"]]))
        self.assertEqual(native["first"], None if py["first"] is None else py["names"][py["first"]])
        for key in ("execute", "frontend", "raw_execute", "outer", "unbalanced"):
            self.assertEqual(native[key], py[key], key)
        for key in ("status", "numerator", "denominator"):
            self.assertEqual(native[key], py["timing"][key], key)
        _, raw, _ = report.load_trace(("a.o", path))
        self.assertEqual(native["raw_frontend"], raw["raw_totals"].get("Frontend", 0))

    def test_all_phases_headers_details_and_raw_provenance(self):
        data = fixture()
        original = copy.deepcopy(data)
        path = self.write(data)
        _, summary, error = report.load_trace(("a.o", path))
        self.assertIsNone(error)
        self.assertEqual(summary["totals"], dict(ExecuteCompiler=600, Frontend=420, Backend=150,
                                                 Source=420, InstantiateClass=60, OptFunction=30))
        self.assertEqual(summary["raw_totals"]["ExecuteCompiler"], 1000)
        self.assertEqual(summary["details"]["InstantiateClass"]["Vec<int>"], (60, 1))
        self.assertEqual(summary["details"]["OptFunction"]["function"], (30, 1))
        self.assertEqual(sum(v[0] for v in summary["families"].values()), 60)
        names, ids, ts, dur = summary["headers"]
        headers = {names[i]: (t, d) for i, t, d in zip(
            array.array("i", ids), array.array("q", ts), array.array("q", dur))}
        self.assertEqual(headers, {"outer.h": (60, 360), "inner.h": (120, 60), "other.h": (480, 60)})
        pch = self.summary(path)
        self.assertEqual(pch["outer"], 420)
        self.assertEqual(dict(zip(pch["names"], pch["own"])),
                         {"outer.h": 300, "inner.h": 60, "other.h": 60})
        self.assertEqual(data, original)
        self.assertEqual(json.loads(self.path.read_text()), original)
        events = list(normalized_events(data))
        self.assertEqual(events[-1]["args"]["count"], 3)
        self.assertEqual(data["beginningOfTime"], 123456789)

    def test_missing_invalid_and_unsupported_metadata(self):
        valid = fixture()["distclang"]
        cases = [(None, "invalid"), ({}, "invalid"),
                 ({**valid, "version": 2}, "unsupported"),
                 ({**valid, "version": True}, "invalid"),
                 ({**valid, "execution_wall_us": 0}, "invalid"),
                 ({**valid, "cpu_user_us": -1}, "invalid"),
                 ({**valid, "cpu_user_us": "500"}, "invalid"),
                 ({**valid, "cpu_user_us": 500.0}, "invalid"),
                 ({**valid, "cpu_user_us": True}, "invalid"),
                 ({**valid, "cpu_user_us": 0, "cpu_system_us": 0}, "invalid"),
                 ({**valid, "cpu_user_us": 2**63}, "invalid"),
                 ({**valid, "cpu_user_us": 2**63 - 1}, "invalid")]
        for metadata, status in cases:
            with self.subTest(metadata=metadata):
                data = fixture()
                data["distclang"] = metadata
                self.assertEqual(trace_timing(data)["status"], status)
                self.assertEqual(list(normalized_events(data)), data["traceEvents"])
                if PROBE:
                    self.parity(self.write(data))
        data = fixture()
        del data["distclang"]
        self.assertEqual(trace_timing(data)["status"], "missing")
        self.assertEqual(self.summary(self.write(data))["execute"], 1000)
        if PROBE:
            self.parity(str(self.path))

    def test_queue_independence_and_cpu_above_wall(self):
        data = fixture()
        data["distclang"]["queue_wait_us"] = 10**12
        self.assertEqual(trace_timing(data)["scale"], 0.6)
        data["distclang"]["cpu_user_us"] = 2000
        self.assertEqual(trace_timing(data)["scale"], 1)
        self.assertEqual(list(normalized_events(data)), data["traceEvents"])
        if PROBE:
            self.parity(self.write(data))

    def test_endpoint_rounding_and_async_complete_equivalence(self):
        data = fixture()
        data["distclang"].update(execution_wall_us=3, cpu_user_us=1, cpu_system_us=0)
        data["traceEvents"] = [
            {"ph": "X", "name": "Source", "ts": 2, "dur": 2, "args": {"detail": "outer.h"}},
            {"ph": "X", "name": "Source", "ts": 3, "dur": 1, "args": {"detail": "inner.h"}},
        ]
        events = list(normalized_events(data))
        self.assertEqual(events[0]["ts"] + events[0]["dur"],
                         events[1]["ts"] + events[1]["dur"])
        complete = self.summary(self.write(data))
        if PROBE:
            self.parity(str(self.path))
        data["traceEvents"] = [
            {"ph": "b", "name": "Source", "ts": 2, "args": {"detail": "outer.h"}},
            data["traceEvents"][1],
            {"ph": "e", "name": "Source", "ts": 4},
        ]
        asynchronous = self.summary(self.write(data))
        self.assertEqual(sorted(zip(complete["names"], complete["parse"])),
                         sorted(zip(asynchronous["names"], asynchronous["parse"])))
        if PROBE:
            self.parity(str(self.path))

    def test_storage_formats_and_native_metadata_order(self):
        data = fixture()
        for before in (False, True):
            if before:
                data = {"distclang": data["distclang"], "traceEvents": data["traceEvents"]}
            raw = json.dumps(data).encode()
            expected = None
            for suffix in ("", ".lz4", ".zst", ".gz"):
                with self.subTest(before=before, suffix=suffix):
                    self.path.write_bytes(raw)
                    if suffix in (".lz4", ".zst"):
                        compile_time_traces.compress_trace(str(self.path), "lz4" if suffix == ".lz4" else "zstd")
                    elif suffix:
                        with gzip.open(str(self.path) + suffix, "wb") as f:
                            f.write(raw)
                    path = str(self.path) + suffix
                    summary = self.summary(path)
                    if expected is None:
                        expected = summary
                    self.assertEqual(summary, expected)
                    if PROBE:
                        self.parity(path)

    def test_build_stats_reader_and_legacy_database(self):
        self.write(fixture())
        traces = build_stats.load_traces(str(self.root), self.paths, [
            dict(kind="compile", pch=False, rule="cxx", output="a.o")], 1)
        self.assertEqual(traces["a.o"]["totals"]["ExecuteCompiler"], 600)
        db = build_stats.Db(str(self.root / "stats.sqlite"))
        db.c.execute("INSERT INTO traces VALUES (1, 'old.o', '//old', 1000, X'')")
        db.c.commit()
        db.c.close()
        db = build_stats.Db(str(self.root / "stats.sqlite"))
        self.addCleanup(db.c.close)
        self.assertEqual(db.c.execute("SELECT compile_us FROM traces").fetchone(), (1000,))
        self.assertEqual(db.c.execute("SELECT count(*) FROM trace_timing").fetchone(), (0,))

    @unittest.skipUnless(PROBE, "TRACE_TIMING_PROBE is not set")
    def test_native_duplicate_metadata_and_trace_keys(self):
        raw = json.dumps(fixture())
        self.path.write_text(raw[:-1] + ',"distclang":{"version":2}}')
        self.parity(str(self.path))
        self.path.write_text(raw[:-1] + ',"traceEvents":[]}')
        self.parity(str(self.path))


if __name__ == "__main__":
    unittest.main()
