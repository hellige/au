### TODO

 - Create a CI build
 - Determine version number from git tag, etc.
 - Make the header-check at start of stream apply to all commands, including
   bisecting `grep` and `zgrep`. Consider adding a flag to disable this check.
 - Add compressed encoding of doubles, either via special-casing common values,
   dictionary or some combination.
 - Add checks to emit empty dict-add record if backref approaches max? (Has
   something like this already been done? I can't remember...)
 - The json parser in the encoder chokes on `nan` rather than `NaN`, but there
   isn't any kind of error. Why?
 - Teach `zgrep` to grep a non-indexed gzip file, and fail only if/when seeking
   is actually required. (This should be a special-case of what `grep` will do
   on a non-seekable stream.)
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
 - seek-buf-size arg to `grep` (for record lookback)
 - Add grepping of content of keys. (This is just a bit different from `-k`...)

### Bugs

 - `grep` can choke on very large records, I believe it's when we seek back to
   the start of the record to re-decode in order to output it.
   Something like that...

### Consider

 - Might be nice to have a slice command:

       au slice -k eventId 123412321321 13412312312
       au slice -k estdEventTime 2018-05-16T14:40:00 2018-05-16T15:40:00

   So far, I haven't really needed this, as I've been able to do everything I
   need by grepping with timestamp truncation and/or context. But it would
   be necessary for certain cases.
 - Combine small-int and varint encoding? Burns another bit in the marker (or
   at least one value)...
