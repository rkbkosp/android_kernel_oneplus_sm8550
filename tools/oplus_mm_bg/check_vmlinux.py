#!/usr/bin/env python3
# Check vmlinux for the userspace strings listed in contract.txt.
# An empty contract passes. A listed string missing from vmlinux fails.
import sys


def load_contract(path):
    needed = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.split("#", 1)[0].strip()
            if line:
                needed.append(line)
    return needed


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: check_vmlinux.py <vmlinux> <contract.txt>\n")
        return 2
    needed = load_contract(argv[2])
    if not needed:
        return 0
    with open(argv[1], "rb") as fh:
        blob = fh.read()
    missing = [item for item in needed if item.encode("utf-8") not in blob]
    if missing:
        for item in missing:
            sys.stderr.write("missing: %s\n" % item)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
