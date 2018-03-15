- TailDictOrder-BadRelOffset01.au
  - Dictionary offset for the first object value corrupted to point a byte
    before the actual dictionary location.
  - Use '-b 92' to start it right before the start of object.
- TailDictOrder-BadRelOffset02.au
  - Dictionary offset for the first object value corrupted to point a byte after
    the actual dictionary location.
  - Use '-b 92' to start it right before the start of object.
- TailDictOrder-BadRelOffset03.au
  - Dictionary offset for the first object value corrupted to point before the
    start of the file.
  - Use '-b 92' to start it right before the start of object.
- Tail-CorruptLength.au
  - The object length has been manually corrupted to be 1 byte longer than the
    real length. Tail should skip it.
  - Use '-b 100' to start tailing before the start of the object.
