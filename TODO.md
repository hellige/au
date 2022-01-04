### TODO

 - Improve docs, especially for the command-line tool.
 - Detect non-json, non-au files, at least simple cases.
 - Add compressed encoding of doubles, either via special-casing common values,
   dictionary or some combination.
 - Add checks to emit empty dict-add record if backref approaches max? (Has
   something like this already been done? I can't remember...)
 - The json parser in the encoder chokes on `nan` rather than `NaN`, but there
   isn't any kind of error. Why?
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
