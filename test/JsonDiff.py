#!/usr/bin/env python

"""
Diff 2 json files containing one JSON object per line.
"""
import argparse
import gzip
import json


def jsonGen(logFile):
    if logFile.endswith('.gz'):
        with gzip.open(logFile, 'r') as f:
            for line in f:
                yield line
    else:
        with open(logFile, 'r') as f:
            for line in f:
                yield line


def diff(file1, file2, outFileName1, outFileName2):
    jg1 = jsonGen(file1)
    jg2 = jsonGen(file2)

    with open(outFileName1, 'w') as out1, open(outFileName2, 'w') as out2:
        while True:
            try:
                l1 = next(jg1)
            except StopIteration:
                return

            try:
                l2 = next(jg2)
            except StopIteration:
                return

            if l1 != l2:
                #out1.write(json.dumps(json.loads(l1), allow_nan=True, indent=2))
                #out1.write("\n");
                #out2.write(json.dumps(json.loads(l2), allow_nan=True, indent=2))
                #out2.write("\n");

                #out1.write(l1)
                #out2.write(l2)
                w1 = l1.split(',')
                w2 = l2.split(',')
                for pr in zip(w1, w2):
                    if pr[0] != pr[1]:
                        out1.write(pr[0] + "\n")
                        out2.write(pr[1] + "\n")
                out1.write("\n")
                out2.write("\n")

def main():
    parser = argparse.ArgumentParser("Audit log combiner")
    parser.add_argument('--in1')
    parser.add_argument('--in2')
    parser.add_argument('--out1')
    parser.add_argument('--out2')

    args = parser.parse_args()
    diff(args.in1, args.in2, args.out1, args.out2)

if __name__ == '__main__':
    main()
