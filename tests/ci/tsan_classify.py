#!/usr/bin/env python3
"""Count ThreadSanitizer reports and the reports that concern EZIO code.

Usage: tsan_classify.py <repo_dir> <report file>...
Prints "<all> <ezio>". A report concerns EZIO when one of its access stacks
(Read/Write/Previous/Atomic) has a frame in an EZIO source file. main.cpp and
app.cpp are not counted: they are only the entry frames of every gRPC thread.
With -v, prints the top EZIO frame of each access of the EZIO reports.
"""
import re
import sys

ENTRY = ("main.cpp:", "app.cpp:")


def classify(repo, files, verbose=False):
    repo = repo.rstrip("/") + "/"
    total = ezio = 0
    for path in files:
        state = None
        with open(path, errors="replace") as f:
            for line in f:
                if line.startswith("WARNING: ThreadSanitizer"):
                    state, stack, hits = "rep", False, []
                elif state != "rep":
                    continue
                elif re.match(r"  (Read|Write|Previous|Atomic)", line):
                    stack = True
                    hits.append(None)
                elif re.match(r"  \S", line):
                    stack = False
                elif stack and re.match(r"\s+#\d+ ", line) and repo in line:
                    src = line.split(repo, 1)[1].split()[0]
                    if not src.startswith(ENTRY) and not src.startswith("tmp/") and hits[-1] is None:
                        hits[-1] = line.split()[1][:70] + " @" + src
                elif line.startswith("SUMMARY: ThreadSanitizer"):
                    total += 1
                    found = [h for h in hits if h]
                    if found:
                        ezio += 1
                        if verbose:
                            print("  " + " || ".join(found))
                    state = None
    return total, ezio


def main():
    args = sys.argv[1:]
    verbose = args and args[0] == "-v"
    if verbose:
        args = args[1:]
    total, ezio = classify(args[0], args[1:], verbose)
    print(total, ezio)


if __name__ == "__main__":
    main()
