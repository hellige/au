#!/bin/bash

zcat $1 | ./au json2au - sample.au
echo Cat
./au cat sample.au | gzip > sample.json.gz
echo Diff
~/drw/algo-research/heimdall-pta/JsonDiff.py --in1 $1 --in2 sample.json.gz --out1 out1.diff --out2 out2.diff
vimdiff out1.diff out2.diff
