#!/usr/bin/env python3
"""Write a large TSV for buffer load/unload stress. Cells are not meant to fit in RAM."""

import argparse
import os
import sys

HOSTS = ("web-1", "web-2", "api-1", "api-2", "worker-a", "worker-b", "cache-1", "db-1")
LEVELS = ("INFO", "INFO", "INFO", "WARN", "DEBUG", "ERROR")
REGIONS = ("us-west", "us-east", "eu-west", "ap-south", "sa-east")
MSGS = (
    "accepted connection",
    "query ok",
    "cache miss",
    "retry scheduled",
    "timeout waiting on upstream",
    "wrote batch",
    "auth ok",
    "rate limited",
    "replica lag",
    "flush complete",
)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("rows", nargs="?", type=int, default=1_000_000)
    p.add_argument("out", nargs="?", default="testdata/stress_records.tsv")
    args = p.parse_args()

    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
    header = "id\tts\thost\tlevel\tlatency_ms\tbytes\tcpu\tregion\tuser\tmsg\n"
    with open(args.out, "w", buffering=1024 * 1024) as f:
        f.write(header)
        buf = []
        for i in range(1, args.rows + 1):
            buf.append(
                "%d\t%d\t%s\t%s\t%d\t%d\t%.2f\t%s\tuser%04d\t%s\n"
                % (
                    i,
                    1_700_000_000 + i * 13,
                    HOSTS[i % len(HOSTS)],
                    LEVELS[i % len(LEVELS)],
                    (i * 7) % 900,
                    64 + (i * 17) % 65536,
                    (i * 13 % 10000) / 100.0,
                    REGIONS[i % len(REGIONS)],
                    i % 5000,
                    MSGS[i % len(MSGS)],
                )
            )
            if len(buf) >= 4000:
                f.writelines(buf)
                buf.clear()
                if i % 200000 == 0:
                    print("wrote %d rows" % i, file=sys.stderr)
        if buf:
            f.writelines(buf)
    size = os.path.getsize(args.out)
    print("wrote %s  rows=%d  bytes=%d" % (args.out, args.rows, size))


if __name__ == "__main__":
    main()
