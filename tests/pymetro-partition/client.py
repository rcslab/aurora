#!/usr/local/bin/python3

import argparse
import os
import socket
import sys


def client(port: int):
    data = bytearray("Hey there", "utf-8")

    datasock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    datasock.connect(("localhost", port))
    datasock.send(data)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Metropolis Client")
    parser.add_argument('port', type=int, default=7778)
    args = parser.parse_args()

    client(args.port)
