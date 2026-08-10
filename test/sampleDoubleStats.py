#!/usr/bin/env python3
"""Sample au.gz files for double-encoding stats.

    sampleDoubleStats.py <listfile> <max-compressed-bytes> <out.jsonl>

Reads at most max-compressed-bytes from the front of each file and
decompresses that. Taking a *compressed* prefix bounds the I/O per file
exactly and yields the compression ratio, so a sample can be extrapolated to
the whole file. Feed the output to aggregateDoubleStats.py.

Set AU to point at the au binary, and MAX_CONCURRENT to control how many files
are read at once (default 4). Reruns skip files already present in out.jsonl.
"""

import json
import os
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

AU = os.environ.get("AU", "au")
# deliberately low: these files tend to live on network storage
MAX_CONCURRENT = int(os.environ.get("MAX_CONCURRENT", "4"))
TIMEOUT = 3600

lock = threading.Lock()
done = 0


def sample(path, maxbytes, out):
    global done
    try:
        size = os.path.getsize(path)
    except OSError as e:
        return f"STAT-FAILED {path}: {e}"

    # head -c the compressed stream, then decompress only what we got
    head = subprocess.Popen(["head", "-c", str(maxbytes), path],
                            stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL)
    zcat = subprocess.Popen(["zcat"], stdin=head.stdout,
                            stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL)
    head.stdout.close()
    au = subprocess.Popen([AU, "stats", "--doubles", "--json", "-"],
                          stdin=zcat.stdout, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)
    zcat.stdout.close()
    try:
        stdout, stderr = au.communicate(timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        au.kill()
        return f"TIMEOUT {path}"
    finally:
        for p in (head, zcat):
            try:
                p.kill()
            except OSError:
                pass
            p.wait()

    text = stdout.decode().strip()
    if not text:
        return (f"NO-OUTPUT {path} (au rc={au.returncode}): "
                f"{stderr.decode().strip()[:200]}")
    try:
        d = json.loads(text)
    except json.JSONDecodeError as e:
        return f"BAD-JSON {path}: {e}; got {text[:200]!r}"

    d["file"] = path
    d["compressedBytes"] = size
    d["compressedRead"] = min(size, maxbytes)
    with lock:
        out.write(json.dumps(d) + "\n")
        out.flush()
        done += 1
        print(f"  [{done}] {d['doubles']:>12,} doubles  "
              f"{d['streamBytes'] / 2**20:8.0f}MB read  {path}",
              file=sys.stderr, flush=True)
    return None


def main():
    listfile, maxbytes, outfile = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    paths = [l.strip() for l in open(listfile) if l.strip()]
    seen = set()
    if os.path.exists(outfile):
        for l in open(outfile):
            try:
                seen.add(json.loads(l)["file"])
            except Exception:
                pass
    todo = [p for p in paths if p not in seen]
    print(f"{len(todo)} to do ({len(paths) - len(todo)} already done), "
          f"max {maxbytes / 2**20:.0f}MB compressed each, "
          f"{MAX_CONCURRENT} at a time",
          file=sys.stderr)

    t0 = time.time()
    with open(outfile, "a") as out:
        with ThreadPoolExecutor(max_workers=MAX_CONCURRENT) as pool:
            for err in pool.map(lambda p: sample(p, maxbytes, out), todo):
                if err:
                    print("  " + err, file=sys.stderr, flush=True)
    print(f"done in {time.time() - t0:.0f}s", file=sys.stderr)


if __name__ == "__main__":
    main()
