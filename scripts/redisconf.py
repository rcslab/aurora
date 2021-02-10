#!/usr/local/bin/python3

import sys
import csv


def tuple_generate(conf_file):
    lines = conf_file.readlines()
    for line in lines:
        if not line.strip().startswith("#"):
            yield line.strip().split()


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python3 config.py <config> <csv>")

    conf_name = sys.argv[1]
    csv_name = sys.argv[2]
    with open(csv_name, 'w') as csv_file:
        csv_writer = csv.writer(csv_file, delimiter=' ')
        with open(conf_name) as conf_file:
            for tup in tuple_generate(conf_file):
                if tup:
                    csv_writer.writerow(tup)
