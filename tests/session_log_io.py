"""Read the text logs of a -brain-debug session directory.

When a block ends, the dedicated server gzip-compresses the block's brain
logs (print2_bot*.log, player*.jsonl, brain_p*.jsonl): <name> becomes
<name>.gz. A session can hold <name>, <name>.gz, or both. When both exist,
the .gz holds the earlier lines and the plain file the later ones.

These helpers take the plain name (for example sess / "print2_bot0.log")
and read whichever files exist.
"""

import gzip
from pathlib import Path


def _parts(path):
    path = Path(path)
    gz = path.with_name(path.name + ".gz")
    return [p for p in (gz, path) if p.is_file()]


def log_exists(path):
    """True when <path> or <path>.gz exists."""
    return bool(_parts(path))


def read_log_text(path, errors="ignore"):
    """All text of <path>.gz then <path>. Empty string when neither exists."""
    out = []
    for p in _parts(path):
        if p.suffix == ".gz":
            with gzip.open(p, "rt", encoding="utf-8", errors=errors) as f:
                out.append(f.read())
        else:
            out.append(p.read_text(encoding="utf-8", errors=errors))
    return "".join(out)


def iter_log_lines(path, errors="replace"):
    """Lines of <path>.gz then <path>, with line ends."""
    for p in _parts(path):
        if p.suffix == ".gz":
            f = gzip.open(p, "rt", encoding="utf-8", errors=errors)
        else:
            f = open(p, encoding="utf-8", errors=errors)
        with f:
            for line in f:
                yield line


def glob_logs(directory, pattern):
    """Plain names of the logs that match pattern (for example
    "print2_bot*.log") in directory, as <name> or as <name>.gz. Sorted."""
    directory = Path(directory)
    names = set()
    for p in directory.glob(pattern):
        names.add(p.name)
    for p in directory.glob(pattern + ".gz"):
        names.add(p.name[:-3])
    return [directory / n for n in sorted(names)]
