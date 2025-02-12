#! /usr/bin/env python3

import sys

if len(sys.argv) < 4:
    raise SystemExit(f"Usage: {sys.argv[0]} input output target [targets...]")

with open(sys.argv[1], "r") as inp:
    with open(sys.argv[2], "w") as out:
        deps = set((line.strip() for line in inp.readlines()))
        print("# Automatically generated, do not edit!", file=out)
        print(" ".join(sys.argv[3:]), ": \\", file=out)
        print(" \\\n".join(deps), file=out)
