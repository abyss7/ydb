"""Find, read and compress compiler time traces (JSON, LZ4, Zstandard, gzip).

The lz4/zstd command-line tools are used by Python; no Python packages are needed.
"""

from contextlib import contextmanager
import gzip
import os
import subprocess
import tempfile


SUFFIXES = ("", ".lz4", ".zst", ".gz")


def find_trace(path):
    """Resolve a JSON path, preferring the newest file (plain JSON on a tie)."""
    found, newest = None, -1
    for suffix in SUFFIXES:
        candidate = path + suffix
        try:
            stamp = os.stat(candidate).st_mtime_ns
        except FileNotFoundError:
            continue
        if stamp > newest:
            found, newest = candidate, stamp
    return found


@contextmanager
def open_trace(path):
    """Read a trace and check the decoder's exit status, including its checksum."""
    decoder = next((tool for suffix, tool in ((".lz4", "lz4"), (".zst", "zstd"))
                    if path.endswith(suffix)), None)
    if decoder is None:
        opener = gzip.open if path.endswith(".gz") else open
        with opener(path, "rt", encoding="utf-8") as stream:
            yield stream
        return
    # stderr goes to a file: a corrupt archive must not deadlock on a full pipe.
    with tempfile.TemporaryFile() as errors:
        with subprocess.Popen([decoder, "-q", "-d", "-c", "--", path],
                              stdout=subprocess.PIPE, stderr=errors,
                              text=True, encoding="utf-8") as process:
            try:
                yield process.stdout
                # A caller may stop before EOF. Still validate the whole frame.
                while process.stdout.read(65536):
                    pass
                if process.wait():
                    errors.seek(0)
                    raise OSError("%s: %s" % (path, errors.read(2048).decode("utf-8", "replace").strip()))
            finally:
                process.stdout.close()
                if process.poll() is None:
                    process.terminate()


def discard_trace(path):
    """Invalidate all formats before recompiling, including cache-hit/failure cases."""
    for suffix in SUFFIXES:
        try:
            os.unlink(path + suffix)
        except FileNotFoundError:
            pass


def compress_trace(path, format):
    """Publish a complete archive atomically; leave JSON intact on any failure."""
    tool, suffix, options = {
        "lz4": ("lz4", ".lz4", ["-1"]),
        "zstd": ("zstd", ".zst", ["-3", "-T1"]),
    }[format]
    stat = os.stat(path)
    fd, temporary = tempfile.mkstemp(prefix=os.path.basename(path) + ".", suffix=".tmp",
                                     dir=os.path.dirname(os.path.abspath(path)))
    try:
        with os.fdopen(fd, "wb") as output:
            subprocess.run([tool, "-q"] + options + ["-c", "--", path],
                           stdout=output, check=True)
        os.chmod(temporary, stat.st_mode & 0o777)
        os.utime(temporary, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        os.replace(temporary, path + suffix)
        os.unlink(path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
