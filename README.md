[![Build status](https://github.com/hellige/au/actions/workflows/ci.yml/badge.svg)](https://github.com/hellige/au/actions?workflow=CI)
[![Code coverage](https://codecov.io/gh/hellige/au/branch/master/graph/badge.svg)](https://codecov.io/gh/hellige/au)
[![GitHub release](https://img.shields.io/github/v/release/hellige/au?include_prereleases&sort=semver)](https://github.com/hellige/au/releases)
[![GitHub license](https://img.shields.io/github/license/hellige/au)](https://github.com/hellige/au/blob/master/LICENSE)

`au` is a file format, header-only C++ library and command-line tool for
working with sequential record-oriented data, primarily log files. The tool
supports grepping in `.au` files, and for convenience supports grepping normal
JSON files and plain timestamped ASCII log files as well. The combination of
binary search within large files, and random access to compressed files
(seekable zstd, or gzip with an index), make this a very useful tool even for
plain ASCII logs!

A rust port of the encoder/decoder library is available
[here](https://github.com/mrettig/au-rs).


## Motivation and usage

Ok, so you're doing some logging. The records have some structure but it's
ragged and irregular, like maybe every line has a timestamp and a couple of
other fields, but beyond that different kinds of events have different fields.
So you decide to use JSON. But now your files start getting really big, the key
names are duplicated all over the place, you have lots of huge ASCII timestamps
all over the place, and it all feels pretty wasteful. Your program is also
spending lots of time formatting JSON, which feels pretty wasteful as well.

But there are a few things that you don't want to lose:
 - structured but schemaless: the records are nicely structured but also
   self-describing. You don't need to update a schema in order to add a new
   record type or otherwise rearrange the content of your logs.
 - greppable, tailable: you can use unix tools to inspect these files in ways
   normal for log files, particularly being able to tail the end of the file
   without having to start at the very beginning.
   
`au` is the tool for you!

Replace your JSON-writing code with calls to `au`. We've observed roughly a 3:1
reduction in file size and a considerable improvement in logging performance.
Using the command-line tool, you can still grep/tail files while they're being
written:

    # decode/follow records as they're appended to the end of the file:
    $ au tail -f mylog.au

    # find records matching a string pattern, include 5 records of context
    # before and after:
    $ au grep -C 5 2018-07-16T08:01:23.102 mylog.au

This is all pretty nice, but let's imagine your files are still annoyingly
large, say 10G.  Grepping is slow, and you need to find things quickly. These
are log files, and all (or most) records have certain useful keys, a timestamp
and and event ID, which happen to be roughly sequentially ordered in the file.

You can find a record containing a particular key/value pair:

    $ au grep -k eventTime 2018-07-16T08:01:23.102 biglog.au
    
which might take a long time. But if you know the values of that key are
roughly ordered, you can also tell `au` to take advantage of that fact by doing
a binary search:

    $ au grep -o eventTime 2018-07-16T08:01:23.102 biglog.au
    
This often reduces a multi-minute grep to 100ms! And you can also request
a specific number of matches, records of context before/after your match, etc.
(see `au grep --help` for details).

`au` also provides the same ability to search within normal JSON files:

    $ au grep -o eventTime 2018-07-16T08:01:23.102 biglog.json

`au` will attempt to automatically detect whether the input stream is JSON
or au-encoded.


### Compressed files

When your files are big enough to be annoying, you'll probably also want to
compress them after writing. `au` reads gzip and zstd files transparently
(`cat`, `grep`, `tail`, ...), but binary search needs random access, which
plain compressed files don't support. There are two ways to get it.

#### Seekable zstd (recommended)

`au zstd` compresses a file in the
[zstd seekable format](https://github.com/facebook/zstd/blob/dev/contrib/seekable_format/zstd_seekable_compression_format.md):
a series of independently compressed frames with a seek table at the end. The
result is an ordinary zstd file that any zstd tool can decompress, but `au`
can jump straight to any frame, so binary search works with no separate index:

    # compress an au file (writes biglog.au.zst):
    $ au zstd --au -T 0 biglog.au
    $ au grep -o eventTime 2018-07-16T08:01:23.102 biglog.au.zst

    # any other file (json, ascii logs) is compressed byte-for-byte:
    $ au zstd -T 0 biglog.json
    $ au grep -o eventTime 2018-07-16T08:01:23.102 biglog.json.zst

`--au` is strongly recommended for au files. It starts every frame with a copy
of the current au dictionary, so each frame can be decoded on its own and
binary search only ever has to look at one frame at a time. This typically
makes `grep -o` 5-10x faster than without it, and well over 10x faster than
gzip, especially on network filesystems. The decoded records are exactly the
same as the original's, but the decompressed *bytes* are not, so don't use
`--au` if you need `zstd -d` to reproduce the original file exactly. Without
`--au`, the output always decompresses to the exact input bytes.

Useful options (see `au zstd --help`):
 - `-T <n>`: compress with `n` threads. `0` means one per CPU the process is
   allowed to use, so `taskset -c 8-15 au zstd -T 0 ...` uses 8 threads, on
   those CPUs.
 - `--cpus <list>`: pin each compression thread to its own CPU, e.g.
   `--cpus 2,3,40-47` (same syntax as `taskset -c`; Linux only). `-T`
   defaults to the number of CPUs listed. This is mainly useful for isolated
   CPUs (`isolcpus`), where the kernel won't spread threads across the CPUs
   in an ordinary affinity mask by itself, so `taskset` alone can leave some
   of them idle.
 - `-l <n>`: compression level (default 9). Higher levels are smaller but
   slower to compress, and are no slower to read. The default takes a few
   seconds per GB with all cores, and on our logs is about 10% smaller than
   zstd's usual default of 3. `-l 19` is another ~13% smaller but around 15x
   slower to compress.
 - `-f <size>`: uncompressed bytes per frame (default 16M). Smaller frames
   make each binary search step a little cheaper but compress slightly worse;
   the default is a good balance.

Note that seekable zstd files can't simply be concatenated (as plain zstd and
gzip files can): the seek table at the end of the result only describes the
last file. `au` detects this and refuses to binary search the result, but you
can still decompress it with `zstd -dc` and recompress, or pipe it through
`au cat -`.

#### Indexed gzip

For existing gzipped files, `au` can build a separate index, after which
binary search works there too:

    # build an index of the file, written to biglog.au.gz.auzx:
    $ au zindex biglog.au.gz
    $ au grep -o eventTime 2018-07-16T08:01:23.102 biglog.au.gz

Building the index requires reading the whole file, and searches are
considerably slower than with seekable zstd.


### Patterns

`au grep` takes advantage of the typed nature of JSON values when possible for
searching. The default is to treat the pattern as possibly any type and try to
match against values of any type, including string values. You can provide
hints on the command line, for example that the pattern should be considered an
integer and only matched against integer values. Special support is provided
for timestamps, because although they aren't a distinct type in JSON, they're
very common in log files. A pattern like:
    
    2018-03-27T18:45:00.123456789
    
will be recognized as a timestamp. Prefixes will match ranges of time, so that
for instance:

    $ au grep -k eventTime 2018-03-27T18:45:00.12 file.au
    
will match any event having an `eventTime` in the given 10ms window, and
`2018-03-27T18` will match events anywhere in the entire hour. Timestamps are
decoded as JSON strings. The encoding library has dedicated functions for
encoding timestamps, while the JSON encoder included in the command-line tool
will recognize strings that happen to be representable as timestamps and encode
them as such.


## Rolling your own decoder

Start with an `AuByteSource`. Use the provided `BufferByteSource` if you have an
in-memory buffer with the `au` data. If you're starting with an on-disk file,
use the `FileByteSourceImpl`. Or inherit from `AuByteSource` if you have more
specialized needs.

In the `src/au/Handlers.h` file you will find a `NoopRecordHandler` and a
`NoopValueHandler` that you can inherit from and override the pieces you're
interested in.

You'll need a value handler like the `NoopValueHandler`. Let's call this
`MyValueHandler`.

You also need a different `ValueHandler` to give to the `au::AuRecordHandler`
(let's call it `OnValueHandler`). This just needs to implement an `onValue()`
that does something like:
```
struct OnValueHandler {
    onValue(AuByteSource &src, Dictionary &dict) {
        MyValueHandler vHandler(dict);
        au::ValueParser parser(src, vHandler);
        parser.value();
    }
};
```

Instead of using `au::AuRecordHandler` directly, you can roll your own (perhaps
by ineriting from `NoopRecordHandler`).

Putting it all together:
```
    au::BufferByteSource auBuf(buf, len);
    au::Dictionary dict;
    OnValueHandler onValueHandler;
    au::AuRecordHandler<OnValueHandler> recHandler(dict, onValueHandler);
    au::RecordParser(auBuf, recHandler).parseStream();
```

The call graph looks like:
`au::RecordParser -> au::RecordHandler -> OnValueHandler -> au::ValueParser -> MyValueHandler`


## Building from source

We use git submodules to include some dependencies. The build is via CMake. You
can set the usual options to control your compiler, build type, etc., but the
crash course is:

    $ git submodule update -i
    $ mkdir -p out/rel
    $ cd out/rel
    $ cmake -DCMAKE_BUILD_TYPE=Release -DSTATIC=On ../..
    $ make
    $ src/au --version

You can run the unit tests in your cmake build directory with:

    $ make unittest

Alternatively, you might use `ctest`.

_Please note that tarballs downloaded from Github releases do not include
submodules, and so building from one of those won't work. Best to just clone
the repo and check out the relevant tag._
