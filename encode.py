#!/usr/bin/env python


import io
import json
import os
import struct
import sys

from collections import OrderedDict

from shared import FORMAT_VERSION

INTERN_THRESH = 10
INTERN_CACHE_SIZE = 10000

# TODO don't intern tiny strings
# TODO build a prefix interner and string pasting operator?
# TODO periodically re-sort and snapshot dictionary based on frequency
# TODO relative or absolute dictionary links?
# TODO could keep dictionary backlink size to 4 bytes by insisting on
#      re-snapshotting the dictionary every 4G


class LimitedSizeDict(OrderedDict):
  def __init__(self, *args, **kwds):
    self.size_limit = kwds.pop("size_limit", None)
    OrderedDict.__init__(self, *args, **kwds)
    self._check_size_limit()

  def __setitem__(self, key, value):
    OrderedDict.__setitem__(self, key, value)
    self._check_size_limit()

  def _check_size_limit(self):
    if self.size_limit is not None:
      while len(self) > self.size_limit:
        self.popitem(last=False)


intern_cache = LimitedSizeDict(size_limit=INTERN_CACHE_SIZE)
dictionary = {}
dict_in_order = []
next_entry = 0
next_dumped_entry = 0


def byte(b): return bytes((b, ))

def encode_int(number, buf):
    while True:
        towrite = number & 0x7f
        number >>= 7
        if number:
            buf.write(byte(towrite | 0x80))
        else:
            buf.write(byte(towrite))
            break


def encode_string(string, buf):
    buf.write(b'S')
    bs = string.encode('utf-8')
    encode_int(len(bs), buf)
    buf.write(bs)


def encode_string_intern(string, buf, force_intern=False):
    global next_entry
    
    if string in dictionary:
        buf.write(b'X')
        encode_int(dictionary[string], buf)
        return

    if string in intern_cache and intern_cache[string] == INTERN_THRESH:
        force_intern = True

    if force_intern:
        intern_cache.pop(string, None)
        dict_in_order.append(string)
        dictionary[string] = next_entry
        buf.write(b'X')
        encode_int(next_entry, buf)
        next_entry += 1
        return

    intern_cache[string] = intern_cache.get(string, 0) + 1
    encode_string(string, buf)


def encode_node(node, buf):
        if isinstance(node, dict):
            buf.write(b'{')
            for k in node:
                encode_string_intern(k, buf, True)
                encode_node(node[k], buf)
            buf.write(b'}')
        elif isinstance(node, list):
            buf.write(b'[')
            for n in node: encode_node(n, buf)
            buf.write(b']')
        elif node is None:
            buf.write(b'N')
        elif isinstance(node, bool):
            buf.write(b'T' if node else b'F')
        elif isinstance(node, str):
            encode_string_intern(node, buf)
        elif isinstance(node, int):
            if node >= 0:
                buf.write(b'I')
                encode_int(node, buf)
            else:
                buf.write(b'J')
                encode_int(-node, buf)
        elif isinstance(node, float):
            buf.write(b'D')
            buf.write(struct.pack('<d', node))
        else:
            raise TypeError("Node has unexpected type for json: %s" % node)


class Buf(object):
    def __init__(self, out):
        self.out = out
        self.pos = 0

    def write(self, bs):
        self.out.write(bs)
        self.pos += len(bs)

    def term(self):
        self.write(b'E')
        self.write(b'\n')


def encode(stream):
    global next_dumped_entry
    fp = Buf(os.fdopen(sys.stdout.fileno(), 'wb'))
    fp.write(b'HI')
    encode_int(FORMAT_VERSION, fp)
    fp.term()
    last_dict = fp.pos
    fp.write(b'C')
    fp.term()
    for line in stream.readlines():
        node = json.loads(line)
        buf = io.BytesIO()
        encode_node(node, buf)
        if next_dumped_entry < next_entry:
            backlink = fp.pos - last_dict
            last_dict = fp.pos
            fp.write(b'A')
            encode_int(backlink, fp)
            while next_dumped_entry < next_entry:
                encode_string(dict_in_order[next_dumped_entry], fp)
                next_dumped_entry += 1
            fp.term()
        backlink = fp.pos - last_dict
        fp.write(b'V')
        encode_int(backlink, fp)
        encode_int(buf.tell(), fp)
        fp.write(buf.getvalue())
        fp.term()


if __name__ == "__main__":
    with open(sys.argv[1], 'r') as stream:
        encode(stream)
