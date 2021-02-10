#!/usr/bin/python3

from sys import argv, stderr, stdout, exit

if __name__ == "__main__":
    if len(argv) != 2:
        print("Usage: ./redisgen.py <#MB of entries>", file=stderr)
        exit(0)

    for i in range(0, 1024 * int(argv[1])):
        cmd = "SET"
        key = str(i)
        value = key * (4096 // len(key))
        stdout.write("*{}\r\n${}\r\n{}\r\n${}\r\n{}\r\n${}\r\n*{}\r\n".
                     format(3, len(cmd), cmd, len(key), key, len(value), value))
