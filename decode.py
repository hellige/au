#!/usr/bin/env python

import collections
import io
import json
import struct
import sys

from shared import FORMAT_VERSION


# TODO support record terminator finding/tail -f mode
# TODO support dictionary rebuilding via backlinks
# TODO grep, etc?
# TODO still null-terminate strings to make `strings` work?

dictionary = []


def decode(file):
    global token, toks
    toks = tokenize(file)
    token = next(toks, None)
    while token.id != "(eof)":
        r = record()
        if r:
            json.dump(r, sys.stdout, separators=(',', ':'))
            print()
        elif isinstance(r, collections.OrderedDict):
            json.dump({}, sys.stdout, separators=(',', ':'))
            print()
        elif isinstance(r, list):
            json.dump([], sys.stdout, separators=(',', ':'))
            print()


def advance(id=None):
    global token
    if id and token.id != id:
        raise SyntaxError("Expected %r" % id)
    token = next(toks, None)


def record():
    global dictionary
    if token.id == 'H':
        advance()
        if token.id != "I":
            raise SyntaxError("Expected version number. Got %s" % token)
        if token.value != FORMAT_VERSION:
            raise SyntaxError("Wrong format version number: %s. Expected %s."
                % (token.value, FORMAT_VERSION))
        advance()
        advance("E")
    elif token.id == 'C':
        advance()
        dictionary = []
        advance("E")
    elif token.id == 'A':
        advance()
        while token.id != "E":
            if token.id != "S":
                raise SyntaxError("Expected a string. Got %s" % token)
            dictionary.append(token.value)
            advance()
        advance("E")
    elif token.id == 'V':
        advance()
        v = value()
        advance("E")
        return v
    else:
        raise SyntaxError("Token not allowed at start of record (%r)." % token.id)


def value():
    t = token
    advance()
    return t.den()


class tok_base(object):
    id = None
    value = None

    def den(self):
        raise SyntaxError("Syntax error (%r)." % self.id)

    def __repr__(self):
        if value:
            return '<%s:%s>' % (self.id, self.value)
        return '<%s>' % self.id

tok_table = {}

def token(id):
    try:
        t = tok_table[id]
    except KeyError:
        class t(tok_base):
            pass
        t.__name__ = "token-" + id # for debugging
        t.id = id
        tok_table[id] = t
    return t


token("(eof)")
token("E") # record terminator
token("C") # dict clear
token("A") # dict add
token("V") # value
token("H") # header/version
token("T").den = lambda self: 'true'
token("F").den = lambda self: 'false'
token("N").den = lambda self: 'null'
token("I").den = lambda self: self.value # integer (64-bit)
token("J").den = lambda self: -self.value # negative integer (64-bit)
token("D").den = lambda self: self.value # double
token("S").den = lambda self: self.value # string
token("X").den = lambda self: dictionary[self.value] # dict idx


# array start
def den(self):
    den = []
    while token.id != "]":
        den.append(value())
    advance("]")
    return den
token("[").den = den
token("]") # array end


# object start
def den(self):
    den = collections.OrderedDict() # maintain ordering!
    while token.id != "}":
        key = value()
        val = value()
        den[key] = val
    advance("}")
    return den
token("{").den = den
token("}") # object end


class FileBytes(object):
    def __init__(self, file, chunksize=8192):
        self.file = file
        self.chunksize = chunksize
        self.pos = 0

    def __iter__(self):
        while True:
            chunk = self.file.read(self.chunksize)
            if chunk:
                for b in chunk:
                    self.pos += 1
                    yield b
            else:
                break


def intbytes(bs):
    shift = 0
    result = 0
    while True:
        i = next(bs, None)
        result |= (i & 0x7f) << shift
        shift += 7
        if not (i & 0x80):
            break
    return result


def tokenize(file):
    ubs = FileBytes(file)
    bs = iter(ubs)
    b = next(bs, None)
    last_dict = 0
    while b:
        b = chr(b)
        if b == "E":
            b = chr(next(bs, None))
            if b != '\n':
                raise SyntaxError("Expected newline! Got %s [%s]" % (ord(b), b))
            s = tok_table["E"]()
            yield s
        elif b in ["C"]:
            s = tok_table["C"]()
            last_dict = ubs.pos - 1
            yield s
        elif b in ["A", "V"]:
            sor = ubs.pos - 1
            s = tok_table[b]()
            backref = intbytes(bs)
            if last_dict + backref != sor:
                raise SyntaxError("Must reload dictionary %s, %s vs %s!" % (last_dict, last_dict + backref, sor)) # TODO
            if b == "A":
                last_dict = sor
            else:
                vLen = intbytes(bs)
            yield s
        elif b in ["X", "I", "J"]:
            s = tok_table[b]()
            s.value = intbytes(bs)
            yield s
        elif b == "D":
            s = tok_table["D"]()
            d = bytearray()
            for i in range(8): d.append(next(bs, None))
            s.value = struct.unpack('<d', d)[0]
            yield s
        elif b == "S":
            s = tok_table["S"]()
            v = io.StringIO()
            length = intbytes(bs)
            for i in range(length):
                b = next(bs, None)
                v.write(chr(b))
            s.value = v.getvalue()
            yield s
        else:
            tok = tok_table.get(b)
            if not tok:
                raise SyntaxError("Unknown token: %s [%s]" % (ord(b), b))
            yield tok()
        b = next(bs, None)
    yield tok_table["(eof)"]()


if __name__ == "__main__":
    with open(sys.argv[1], 'rb') as stream:
        decode(stream)
