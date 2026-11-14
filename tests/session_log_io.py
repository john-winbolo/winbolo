"""Read the text logs of a -brain-debug session directory.

When a block ends, the dedicated server compresses the block's brain logs:
print2_bot*.log becomes <name>.zst (zstd with a long window; older builds
wrote <name>.gz), and player*.jsonl and brain_p*.jsonl become <name>.gz.
A session can hold <name>, a compressed copy, or both. When both exist,
the compressed copy holds the earlier lines and the plain file the later
ones.

These helpers take the plain name (for example sess / "print2_bot0.log")
and read whichever files exist. A .zst file needs the Python "zstandard"
package or the "zstd" program on PATH.
"""

import gzip
import io
import shutil
import subprocess
from pathlib import Path

_EXTS = (".zst", ".gz")


def _parts(path):
    path = Path(path)
    packed = [path.with_name(path.name + e) for e in _EXTS]
    return [p for p in (*packed, path) if p.is_file()]


def _zst_bytes(p):
    """The decompressed bytes of a .zst file."""
    try:
        import zstandard  # noqa: PLC0415 - optional
    except ImportError:
        zstandard = None
    if zstandard is not None:
        with open(p, "rb") as f:
            # The logs use a window of up to 128 MiB (windowLog 27).
            d = zstandard.ZstdDecompressor(max_window_size=1 << 27)
            with d.stream_reader(f) as r:
                return r.read()
    exe = shutil.which("zstd")
    if exe is None:
        raise RuntimeError(f"{p}: reading a .zst log needs the Python "
                           "'zstandard' package or the 'zstd' program")
    return subprocess.run([exe, "-dc", "--long=27", str(p)], check=True,
                          stdout=subprocess.PIPE).stdout


def _open_text(p, errors):
    if p.suffix == ".gz":
        return gzip.open(p, "rt", encoding="utf-8", errors=errors)
    if p.suffix == ".zst":
        return io.StringIO(_zst_bytes(p).decode("utf-8", errors=errors))
    return open(p, encoding="utf-8", errors=errors)


def log_exists(path):
    """True when <path>, <path>.zst or <path>.gz exists."""
    return bool(_parts(path))


def read_log_text(path, errors="ignore"):
    """All text of <path>.zst, <path>.gz, then <path>. Empty string when
    none exists."""
    out = []
    for p in _parts(path):
        with _open_text(p, errors) as f:
            out.append(f.read())
    return "".join(out)


def iter_log_lines(path, errors="replace"):
    """Lines of <path>.zst, <path>.gz, then <path>, with line ends."""
    for p in _parts(path):
        with _open_text(p, errors) as f:
            for line in f:
                yield line


def glob_logs(directory, pattern):
    """Plain names of the logs that match pattern (for example
    "print2_bot*.log") in directory, as <name>, <name>.zst or <name>.gz.
    Sorted."""
    directory = Path(directory)
    names = set()
    for p in directory.glob(pattern):
        names.add(p.name)
    for e in _EXTS:
        for p in directory.glob(pattern + e):
            names.add(p.name[:-len(e)])
    return [directory / n for n in sorted(names)]
