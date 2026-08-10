#!/usr/bin/env python3
"""Aggregate `au stats --doubles --json` output across many files.

`au stats --doubles --json` emits one json object per input file. Feed those
objects (from as many runs as you like) to this script on stdin to get a single
combined picture of a whole corpus:

    find /path/to/logs -name '*.au' -print0 \\
      | xargs -0 -P 16 -n 32 au stats --doubles --json \\
      | test/aggregateDoubleStats.py

Pass --by-file to also get a per-file summary line, which is useful for
spotting applications whose files behave differently from the rest.
"""

import json
import sys
from collections import defaultdict

SCALARS = [
    "streamBytes", "records", "recordsWithDoubles", "doubles",
    "bytesToday", "bytesTierA", "bytesTierAB", "bytesTierABC",
    "zero", "negZero", "nonFinite", "float32", "raw", "decimalNotWorthIt",
    "repeatOfPrevious", "repeatOfEarlier", "repeatBeyondWindow",
    "doublesInRuns", "xorPairsRecord", "xorPairsArray",
]

VECTORS = [
    "byScale", "significandVarintLen", "rawTrailingZeroBytes",
    "doublesPerRecord", "longestArrayRun",
    "xorSigBytesRecord", "xorSigBytesArray",
]


def pct(part, whole):
    return 100.0 * part / whole if whole else 0.0


def bar(frac, width=40):
    n = int(round(frac * width))
    return "#" * n + "." * (width - n)


def main():
    totals = defaultdict(int)
    vectors = {name: defaultdict(int) for name in VECTORS}
    values = defaultdict(int)
    keys = defaultdict(lambda: defaultdict(int))
    files = []
    skipped = 0

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            d = json.loads(line)
        except json.JSONDecodeError:
            skipped += 1
            continue
        files.append(d)
        for name in SCALARS:
            totals[name] += d.get(name, 0)
        for name in VECTORS:
            for i, v in enumerate(d.get(name, [])):
                vectors[name][i] += v
        for entry in d.get("topValues", []):
            values[entry["bits"]] += entry["count"]
        for entry in d.get("topKeys", []):
            k = keys[entry["key"]]
            for f in ("count", "decimal", "float32", "raw", "bytesTierA"):
                k[f] += entry.get(f, 0)

    if not files:
        print("no input", file=sys.stderr)
        return 1
    if skipped:
        print(f"warning: skipped {skipped} unparseable line(s)",
              file=sys.stderr)

    n = totals["doubles"]
    stream = totals["streamBytes"]
    today = totals["bytesToday"]

    print(f"Files:            {len(files):,}")
    print(f"Stream bytes:     {stream:,}")
    print(f"Records:          {totals['records']:,}"
          f" ({pct(totals['recordsWithDoubles'], totals['records']):.1f}%"
          f" contain a double)")
    print(f"Doubles:          {n:,}")
    if not n:
        print("\nNo doubles in this corpus. Nothing to compress.")
        return 0

    print(f"Bytes in doubles: {today:,} ({pct(today, stream):.1f}% of stream)")
    print()
    print("Projected savings, as a fraction of the whole stream:")
    for label, key in (("(a)     per-value tiers", "bytesTierA"),
                       ("(a+b)   + record repeats", "bytesTierAB"),
                       ("(a+b+c) + xor delta", "bytesTierABC")):
        saved = today - totals[key]
        print(f"  {label:26s} {totals[key]:>15,} bytes"
              f"  saves {pct(saved, stream):5.1f}% of stream"
              f"  ({pct(saved, today):5.1f}% of doubles)")

    print()
    print("Tier mix:")
    decimal = (n - totals["zero"] - totals["negZero"] - totals["nonFinite"]
               - totals["float32"] - totals["raw"])
    tiers = [
        ("+0.0", totals["zero"]),
        ("-0.0", totals["negZero"]),
        ("NaN / Inf", totals["nonFinite"]),
        ("decimal", decimal),
        ("float32 exact", totals["float32"]),
        ("raw", totals["raw"]),
        ("  of which decimal-but-bigger", totals["decimalNotWorthIt"]),
    ]
    for label, count in tiers:
        f = count / n
        print(f"  {label:30s} {count:>14,}  {pct(count, n):5.1f}%  {bar(f)}")

    print()
    print("Decimal scale distribution:")
    for i in sorted(vectors["byScale"]):
        c = vectors["byScale"][i]
        if not c:
            continue
        note = "" if i <= 7 else "  (needs extended form)"
        print(f"  scale {i:<3d} {c:>14,}  {pct(c, n):5.1f}%{note}")

    print()
    print("Record shape (doubles per record):")
    per = vectors["doublesPerRecord"]
    recs = totals["records"]
    # the last bucket is an overflow bucket: "more than MAX_BUCKET"
    overflow = max(per) if per else 0
    for i in sorted(per):
        if not per[i]:
            continue
        label = f">{overflow - 1}" if i == overflow else str(i)
        print(f"  {label:>5s} {per[i]:>14,}  {pct(per[i], recs):5.1f}%")

    print()
    print("Record-local exact repeats:")
    for label, key in (("same as previous", "repeatOfPrevious"),
                       ("same as earlier", "repeatOfEarlier"),
                       ("beyond backref window", "repeatBeyondWindow")):
        print(f"  {label:30s} {totals[key]:>14,}  "
              f"{pct(totals[key], n):5.1f}%")

    for which, key, pairs in (("record-wide", "xorSigBytesRecord",
                               totals["xorPairsRecord"]),
                              ("array runs only", "xorSigBytesArray",
                               totals["xorPairsArray"])):
        if not pairs:
            continue
        print()
        print(f"XOR significant bytes, {which} ({pairs:,} pairs):")
        for i in sorted(vectors[key]):
            c = vectors[key][i]
            if not c:
                continue
            win = " (smaller than raw)" if i <= 6 else ""
            print(f"  {i} bytes {c:>14,}  {pct(c, pairs):5.1f}%{win}")

    print()
    print("Most frequent values across the corpus"
          " (from per-file top-N, so indicative only):")
    import struct
    for bits, count in sorted(values.items(), key=lambda kv: -kv[1])[:20]:
        d = struct.unpack("<d", struct.pack("<Q", int(bits, 16)))[0]
        print(f"  {count:>14,}  {d!r}")

    print()
    print("Keys holding the most doubles"
          " (from per-file top-N, so indicative only):")
    print(f"  {'count':>14s}  {'dec':>8s} {'f32':>8s} {'raw':>8s}"
          f"  {'saved':>12s}  key")
    for key, k in sorted(keys.items(), key=lambda kv: -kv[1]["count"])[:25]:
        saved = k["count"] * 9 - k["bytesTierA"]
        print(f"  {k['count']:>14,}  {k['decimal']:>8,} {k['float32']:>8,}"
              f" {k['raw']:>8,}  {saved:>12,}  {key or '<no key>'}")

    if "--by-file" in sys.argv:
        print()
        print("Per file:")
        for d in sorted(files, key=lambda x: -x.get("doubles", 0)):
            if not d.get("doubles"):
                continue
            saved = d["bytesToday"] - d["bytesTierABC"]
            print(f"  {pct(saved, d['streamBytes']):5.1f}% of stream"
                  f"  {d['doubles']:>12,} doubles  {d['file']}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
