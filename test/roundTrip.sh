#!/bin/bash

D=samples
mkdir -p $D

zcat $1 | ./au json2au - $D/sample.au
echo Cat
./au $D/sample.au | gzip > $D/sample.json.gz
echo JsonDiff
test/JsonDiff.py --in1 $1 --in2 $D/sample.json.gz --out1 $D/out1.diff --out2 $D/out2.diff
echo VimDiff
vimdiff $D/out1.diff $D/out2.diff
