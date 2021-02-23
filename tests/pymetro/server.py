#!/usr/local/bin/python3

import argparse
import os
import socket
import sys


def server(port: int):

    # Set up listening socket
    listensock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listensock.bind(("localhost", port))
    listensock.listen(512)

    # Wait for a connection (this is where Metropolis creates the image)
    (datasock, address) = listensock.accept()

    data = datasock.recv(4096)
    msg = data.decode("utf-8")
    if msg != "Hey there":
        print("Test fails, message is {}".format(msg))
        sys.exit(-1)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Metropolis Server")
    parser.add_argument('port', type=int, default=7779)
    args = parser.parse_args()

    server(args.port)
