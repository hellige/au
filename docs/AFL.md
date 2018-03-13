Fuzz-testing under AFL
----------------------

`au` is pretty amenable to fuzz testing. To test under AFL:

* Download AFL (http://lcamtuf.coredump.cx/afl/) and build it (a simple `make` does the trick)
* Create a toolchain pointing at the afl-g++ and afl-gcc built above. In CLion, that means making a new
  toolchain and selecting those two executables. From the command-line you'll need to supply `-DCMAKE_C_COMPILER` etc
* Build a debug build using the AFL toolchain. You'll see output from the AFL system during the build.
* Now create some example `.au` files, say in `/tmp/cases`. One or two is enogh, each no more than a few hundred KB.

Finally you can run AFL fuzz:

```bash
$ path/to/afl/afl-fuzz -i /tmp/cases -o /tmp/afl-output -m none --  path/to/afl/built/au cat
```

NB it takes a LOOONG time. You'll get results pretty quickly though. Sample files that cause crashes are placed in the output directory (`/tmp/afl-output` above).
