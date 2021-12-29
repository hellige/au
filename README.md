[![Build status](https://github.com/hellige/au/actions/workflows/ci.yml/badge.svg)](https://github.com/hellige/au/actions?workflow=CI)
[![Code coverage](https://codecov.io/gh/hellige/au/branch/master/graph/badge.svg)](https://codecov.io/gh/hellige/au)
[![GitHub release](https://img.shields.io/github/v/release/hellige/au?include_prereleases&sort=semver)](https://github.com/hellige/au/releases)
[![GitHub license](https://img.shields.io/github/license/hellige/au)](https://github.com/hellige/au/blob/master/LICENSE)

`au` is a file format, header-only C++ library and command-line tool for
working with sequential record-oriented data, primarily log files. The tool
supports grepping in `.au` files, and for convenience supports grepping normal
JSON files as well. The combination of binary search within large json files,
and the ability to index and offer random access to gzipped files, make this a
very useful tool even for plain ASCII JSON files!


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
compress them after writing.  In order to keep supporting binary search, `au`
can read and index gzipped files, after which binary search is supported there
too:

    # build an index of the file, written to biglog.au.gz.auzx:
    $ au zindex biglog.au.gz

    # note grep is now zgrep! this is still a binary search:
    $ au zgrep -o eventTime 2018-07-16T08:01:23.102 biglog.au.gz

And, once again, we can do this with normal JSON files as well:

    $ au zindex biglog.json.gz
    $ au zgrep -o eventTime 2018-07-16T08:01:23.102 biglog.json.gz


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

    $ git submdoule update -i
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
