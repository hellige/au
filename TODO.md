### TODO

 - Improve docs, especially for the command-line tool.
 - Detect non-json, non-au files, at least simple cases.
 - Compressed encoding of doubles: measured, and not worth doing. See
   "Compressed doubles" below.
 - The json parser in the encoder chokes on `nan` rather than `NaN`, but there
   isn't any kind of error. Why? Note it's worse than that: `au enc` on a
   document containing `NaN` writes a *malformed* record (an object start with
   no key, no matching end, and a length that doesn't match), which then can't
   be decoded at all. Reproduce with `echo '{"a":NaN}' | au enc`.
 - Teach `tail` to do `n` records rather than bytes from end.
 - `stats`: count pos/neg int representations
 - Configurable encoding:
   - Parameters args to `enc` and `-e`?
   - Allow to intern small strings?
   - Allow to intern doubles (see double-encoding above)?
   - Allow to pre-populate dictionary and control dictionary flush/rebuild
     logic?
 - `-e` arg to `tail`
 - scan-buf-size arg to `grep` (for bisect)
 - Add grepping of content of keys. (This is just a bit different from `-k`...)
 - The gzip handling code has an extra layer of buffering. There's the buffer
   in the FileByteSource and then an `output_` buffer in ZipByteSource, and
   data is pointlessly copied between. This is an artifact of lifting the gzip
   code from `zindex`, but it would be good to clean that up and eliminate the
   intermediate buffer.
 - Get rid of the pure virtual AuByteSource and make BufferByteSource a subtype
   like the other two. The buffer case is much less common, and I don't think
   it's worth adding lots of virtual calls in the common case just to keep that
   code simple.

### Consider

 - It would be nice for `au` to be able to run a persistent daemon (or some such
   approach) to be able to support a large number of repeated binary searches
   into the same file for values of the same key. By incrementally building a
   binary search tree, we should be able to get a nice speedup, not to mention
   avoiding the overhead of loading the index every single time, etc.
 - Automatically build in-memory index when binary searching non-indexed gzip
   file? This is potentially very time-consuming, so might not be a good idea
   to do it transparently. But with a command-line option, maybe?
 - Might be nice to have a slice command:

       au slice -k eventId 123412321321 13412312312
       au slice -k estdEventTime 2018-05-16T14:40:00 2018-05-16T15:40:00

   So far, I haven't really needed this, as I've been able to do everything I
   need by grepping with timestamp truncation and/or context. But it would
   be necessary for certain cases.
 - Combine small-int and varint encoding? Burns another bit in the marker (or
   at least one value)...

### Compressed doubles

Doubles are stored as a marker plus 8 raw bytes. Compressing them was measured
against a large input corpus (~2000 files, 300GB compressed, 1.40TB of au data)
with `au stats --doubles`, and the answer was no.

Whole corpus:

|                                  | bytes   | share of stream |
|----------------------------------|---------|-----------------|
| doubles, as stored today         | 12.9 GB |           0.90% |
| saving, per-value tiers          | 8.64 GB |           0.60% |
| saving, + record-local repeats   | 9.80 GB |           0.69% |
| saving, + record-local xor delta | 9.84 GB |           0.69% |

Doubles are simply too rare to matter, in our corpus. Only 5% of our files
contain any double values at all, and in those files doubles are still only
3.9% of bytes.

Where they do occur, the scheme works well (73% of double bytes would go away)
so if this is ever revisited for a corpus that really is double-heavy, it's
worth remembering:

 - A decimal tier (significand / 10^scale, verified bit-exact against the
   decoder's reconstruction, falling back to raw when it wouldn't be smaller)
   covers 70% of values, 99.97% of which need a scale of 5 or less. Eight scale
   codes in the marker is plenty; an extended form is nearly dead weight.
 - Record-local back-references to an identical earlier double in the same
   record are the second most valuable mechanism, taking the saving from 62% to
   73% of double bytes. 45% of doubles are identical to the immediately
   preceding one. This is cheap: a value record is always decoded from its own
   start, so the state is trivially reconstructible.
 - Byte-granular XOR against the preceding double is worthless: 73.1% vs 72.9%.
   Half of all adjacent pairs are already identical (caught above), and of the
   rest most differ in 7 or 8 of their 8 bytes. Bit-granular Gorilla-style
   packing would do better but conflicts with au being byte-aligned throughout.
 - Do not expect a bigger win on the gzipped size. The intuition that raw IEEE
   doubles are incompressible does not hold here: only 6.5% of the doubles are
   distinct values, and the top one (0.1) accounts for 40 million occurrences,
   so gzip already handles them well.

Anything that predicts a double from *previous records* -- Gorilla, delta of
delta, a stream-level double dictionary -- is off the table regardless of the
numbers. A reader must be able to decode a value record found at an arbitrary
offset, which is what `tail`, `grep -o` bisect, and grep's context rewind all
depend on. The only reconstructible cross-value state is within a single
record.
