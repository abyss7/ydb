"""Trace storage integration tests; PCH_TEST_CLANG selects the native compiler.

PCH_UPDATE_TEST_BINARY optionally enables the native reader cross-checks.
"""

import gzip
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import build_time_report as report
import pch_target_header
import post_compile
import compile_time_traces


CLANG = os.environ.get("PCH_TEST_CLANG", "clang++")
NATIVE = os.environ.get("PCH_UPDATE_TEST_BINARY")
LAUNCHER = str(Path(__file__).resolve().with_name("post_compile.py"))


class TraceTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="trace test ")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.build = self.root / "build dir"
        self.build.mkdir()
        self.source = self.root / "a.cc"
        self.source.write_text("struct A { int value; }; int f() { return sizeof(A); }\n")
        self.trace = str(self.build / "a.json")

    def compile(self, format, extra=(), pch=False, check=True):
        command = [sys.executable, LAUNCHER, str(self.root),
                   "--time-trace=" + format, "--", CLANG]
        if format != "none":
            command += ["-ftime-trace", "-ftime-trace-granularity=0"]
        if pch:
            command += ["-x", "c++-header"]
        command += list(extra) + ["-MD", "-MF", "a.d", "-c", "../a.cc", "-o", "a.pch" if pch else "a.o"]
        result = subprocess.run(command, cwd=self.build, capture_output=True, text=True)
        if check:
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn("post_compile:", result.stderr)
        return result

    def test_compile_formats_and_switching(self):
        for format, suffix in (("none", None), ("lz4", ".lz4"), ("zstd", ".zst"), ("json", "")):
            with self.subTest(format=format):
                self.compile(format)
                if suffix is None:
                    self.assertIsNone(compile_time_traces.find_trace(self.trace))
                    continue
                path = compile_time_traces.find_trace(self.trace)
                self.assertEqual(path, self.trace + suffix)
                self.assertEqual(list(self.build.glob("a.json*")), [Path(path)])
                with compile_time_traces.open_trace(path) as stream:
                    self.assertIn("traceEvents", json.load(stream))
                self.assertEqual(report.trace_path_for(str(self.build), "a.o"), path)

    def test_c_compilation(self):
        self.source.write_text("int f(void) { return 7; }\n")
        self.compile("zstd", extra=("-x", "c"))
        with compile_time_traces.open_trace(self.trace + ".zst") as stream:
            self.assertIn("traceEvents", json.load(stream))

    def test_pch_and_explicit_trace_path(self):
        for extra, name in (((), "a.json"), (("-ftime-trace=explicit.json",), "explicit.json"),
                            (("-ftime-trace=traces/",), "traces/a.json"),
                            (("-dumpdir", "traces/"), "traces/a.json")):
            with self.subTest(extra=extra):
                (self.build / "traces").mkdir(exist_ok=True)
                self.compile("lz4", extra=extra, pch=True)
                path = str(self.build / name) + ".lz4"
                with compile_time_traces.open_trace(path) as stream:
                    self.assertIn("traceEvents", json.load(stream))
                self.assertFalse((self.root / name).exists())

    def test_compile_failure_drops_old_trace(self):
        self.compile("lz4")
        self.source.write_text("#error expected compile failure\n")
        result = self.compile("json", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIsNone(compile_time_traces.find_trace(self.trace))

    def test_cache_hit_does_not_reuse_old_trace(self):
        self.compile("zstd")
        command = [sys.executable, LAUNCHER, str(self.root), "--time-trace=lz4", "--",
                   "/bin/true", "-ftime-trace", "-o", "a.o"]
        result = subprocess.run(command, cwd=self.build, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0)
        self.assertIn("missing time trace", result.stderr)
        self.assertIsNone(compile_time_traces.find_trace(self.trace))

    def test_compression_failure_retains_raw_and_cleans_temporary(self):
        Path(self.trace).write_text('{"traceEvents": []}')
        with mock.patch.object(compile_time_traces.subprocess, "run", side_effect=OSError("no compressor")):
            with self.assertRaises(OSError):
                compile_time_traces.compress_trace(self.trace, "lz4")
        self.assertEqual(list(self.build.iterdir()), [Path(self.trace)])
        self.assertEqual(json.loads(Path(self.trace).read_text()), {"traceEvents": []})

    def test_newest_trace_and_raw_tie(self):
        Path(self.trace).write_text("{}")
        Path(self.trace + ".zst").write_bytes(b"old")
        os.utime(self.trace + ".zst", ns=(10, 10))
        os.utime(self.trace, ns=(20, 20))
        self.assertEqual(compile_time_traces.find_trace(self.trace), self.trace)
        os.utime(self.trace + ".zst", ns=(30, 30))
        self.assertEqual(compile_time_traces.find_trace(self.trace), self.trace + ".zst")
        os.utime(self.trace, ns=(30, 30))
        self.assertEqual(compile_time_traces.find_trace(self.trace), self.trace)

    def fixture(self):
        # More than one decompression buffer, with nested header events.
        return json.dumps({"traceEvents": [
            {"name": "Source", "ph": "X", "ts": i * 100, "dur": 80,
             "args": {"detail": "header%d.h" % (i % 23)}} for i in range(3000)]}).encode()

    def variants(self, data):
        Path(self.trace).write_bytes(data)
        yield self.trace
        for format, suffix in (("lz4", ".lz4"), ("zstd", ".zst")):
            Path(self.trace).write_bytes(data)
            stamp = os.stat(self.trace).st_mtime_ns
            compile_time_traces.compress_trace(self.trace, format)
            self.assertEqual(os.stat(self.trace + suffix).st_mtime_ns, stamp)
            yield self.trace + suffix
        with gzip.open(self.trace + ".gz", "wb") as stream:
            stream.write(data)
        yield self.trace + ".gz"

    def native_summary(self, path):
        result = self.build / "native.bin"
        spec = "=" + "".join("%%%02X" % ord(c) if c in "% \t\r\n" else c for c in path)
        Path(str(result) + ".rsp").write_text(spec)
        subprocess.run([NATIVE, "job", "trace", str(result)], check=True, capture_output=True)
        data = result.read_bytes()
        offset = 0

        def integer():
            nonlocal offset
            value, shift = 0, 0
            while True:
                byte = data[offset]
                offset += 1
                value |= (byte & 0x7f) << shift
                if byte < 0x80:
                    return value
                shift += 7

        integer()  # result tag
        kind_size = integer()
        self.assertEqual(data[offset:offset + kind_size], b"trace")
        offset += kind_size
        ok = integer()
        return ok, data

    def test_readers_agree_for_all_formats(self):
        data = self.fixture()
        expected_report, expected_native = None, None
        report._init_worker(report.Paths(str(self.build), str(self.root), None))
        for path in self.variants(data):
            with self.subTest(path=path):
                with compile_time_traces.open_trace(path) as stream:
                    self.assertEqual(json.load(stream), json.loads(data))
                summary = report.load_trace(("a.o", path))
                self.assertIsNone(summary[2])
                if expected_report is None:
                    expected_report = summary
                self.assertEqual(summary, expected_report)
                self.assertEqual(pch_target_header.headers(path, str(self.build)), {})
                if NATIVE:
                    ok, native = self.native_summary(path)
                    self.assertTrue(ok)
                    if expected_native is None:
                        expected_native = native
                    self.assertEqual(native, expected_native)

    def test_truncated_and_corrupt_archives_are_rejected(self):
        for path in self.variants(self.fixture()):
            if path == self.trace:
                continue
            original = Path(path).read_bytes()
            for content in (original[:-1], original[:len(original) // 2],
                            original[:-1] + bytes([original[-1] ^ 0xff]), b""):
                with self.subTest(path=path, size=len(content)):
                    Path(path).write_bytes(content)
                    with self.assertRaises((OSError, EOFError, json.JSONDecodeError)):
                        with compile_time_traces.open_trace(path) as stream:
                            json.load(stream)
                    if NATIVE:
                        ok, _ = self.native_summary(path)
                        self.assertFalse(ok)


if __name__ == "__main__":
    unittest.main()
