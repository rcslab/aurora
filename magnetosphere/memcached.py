#!/usr/bin/env python3

import click
import csv
from configparser import ConfigParser
import multiprocessing as mp
from pathlib import Path
import os
import signal
import sys
import time
from typing import Dict, List

from sls import bashcmd
import sls


def create_paths(chroot: Path):
    os.makedirs(chroot / "log" / "memcached", exist_ok=True)
    os.makedirs(chroot / "var" / "cache" / "memcached", exist_ok=True)
    os.makedirs(chroot / "var" / "run" / "memcached", exist_ok=True)
    os.makedirs(chroot / "data", exist_ok=True)


def execute_memcached(memcached: str, options: Dict[str, str], root: str):

    os.chroot(root)
    args = [options[key] for key in options if key != "binary"]
    memcached_command = [memcached]
    for arg in args:
        memcached_command += list(arg.split(" "))

    print(memcached_command)
    os.execv(memcached, memcached_command)


def execute_dtrace(dscript: str) -> None:
    os.execv(dscript, ["dtrace"])


# Tried being Pythonic with atexit, didn't work, got fed up.
memcached_process = None
dtrace_process = None
root = None
teardown_root = False


def teardown(signum: int, stack) -> None:
    global memcached_process
    global dtrace_process
    global teardown_root
    global root

    if dtrace_process is not None:
        dtrace_process.terminate()
        dtrace_process.join()

    if memcached_process is not None:
        memcached_process.kill()
        memcached_process.join()

    bashcmd(["sysctl", "aurora"])

    if teardown_root:
        sls.teardown(root)
    sys.exit(0)


@click.command()
@click.argument("inifile")
@click.argument("extra_options", nargs=-1)
def memcached(inifile, extra_options) -> None:
    global memcached_process
    global dtrace_process
    global teardown_root
    global root

    signal.signal(signal.SIGINT, teardown)

    options = ConfigParser()
    options.read(inifile)

    # Apply any option overrides by Magnetosphere.
    for option in extra_options:
        section, pair = option.split(".")
        key, value = pair.split("=")
        options[section][key] = value

    if options["stripe"]["enabled"] == "yes":
        name = options["stripe"]["name"]
        size = int(options["stripe"]["size"])
        disks = options["stripe"]["disks"].split()
        sls.gstripe(name, size, disks)

    memcached = options["memcached"]["binary"]
    dscript = str(Path(options["sls"]["sls_source"], options["sls"]["dtrace"]))
    root = Path(options["sls"]["mount_point"])

    # If this script is setting up the root, ensure it will be torn down in the
    # end.
    if options["magnetosphere"]["setup-root"] == "yes":
        sls.load_modules(Path(options["sls"]["sls_source"]))
        sls.create_root(options)
        teardown_root = True

    if options["sls"]["enabled"] == "yes":
        dtrace_process = mp.Process(target=execute_dtrace, args=(dscript,))
        dtrace_process.start()

    create_paths(root)

    memcached_process = mp.Process(target=execute_memcached, args=(
        memcached, options["memcached"], str(root)))
    memcached_process.start()

    if options["sls"]["enabled"] == "yes":
        print("Checkpointing {} with period {}".format(
            memcached_process.pid, options["sls"]["PERIOD"]))
        sls.checkpoint(memcached_process.pid, options["sls"])

    # Wait for a signal to kill us.
    while True:
        time.sleep(1)


if __name__ == "__main__":
    memcached()
