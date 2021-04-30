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


def uncomment(row: List[str], keywords: List[str]) -> List[str]:
    if not row or len(row) < 2:
        return row

    if row[0] != "#":
        return row

    if row[1] in keywords:
        return row[1:]

    return row


def generate_config_file(chroot: Path, options: Dict[str, str]) -> None:
    """ Generate a new server configfile from a Magnetosphere ini file."""

    uncomment_keywords = options["UNCOMMENT"].split(",")

    new_config = ""
    with open(options["template"], 'r', newline='') as csvfile:
        optreader = csv.reader(csvfile, delimiter=" ", quoting=csv.QUOTE_ALL)
        for row in optreader:
            row = uncomment(row, uncomment_keywords)
            if not row:
                new_config += "\n"
                continue

            if row[0] in options:
                if len(row) != 1:
                    row[1] = options[row[0]]
                else:
                    row.append(options[row[0]])
                row = row[0:2]

            new_config += " ".join(row)
            new_config += "\n"

    config_path = chroot / Path(options["conf"]).relative_to("/")
    print("Configuration file written in {} for chroot {}".format(config_path,
                                                                  chroot))
    os.makedirs(config_path.parent, exist_ok=True)
    with open(config_path, "w") as config_file:
        config_file.write(new_config)


def create_paths(chroot: Path):
    os.makedirs(chroot / "var" / "db" / "redis", exist_ok=True)


def execute_redis(redis: str, conf: str, root: str = "/") -> None:
    os.chroot(root)
    os.execv(redis, ["redis-server", conf])


def execute_dtrace(dscript: str) -> None:
    os.execv(dscript, ["dtrace"])


# Tried being Pythonic with atexit, didn't work, got fed up.
redis_process = None
dtrace_process = None
root = None
teardown_root = False


def teardown(signum: int, stack) -> None:
    global redis_process
    global dtrace_process
    global teardown_root
    global root

    if dtrace_process is not None:
        dtrace_process.terminate()
        dtrace_process.join()

    if redis_process is not None:
        redis_process.kill()
        redis_process.join()

    bashcmd(["sysctl", "aurora"])

    if teardown_root:
        sls.teardown(root)
    sys.exit(0)


@click.command()
@click.argument("inifile")
@click.argument("extra_options", nargs=-1)
def redis_server(inifile, extra_options) -> None:
    global redis_process
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

    redis = options["redis-server"]["binary"]
    conf = options["redis-server"]["conf"]
    dscript = Path(options["sls"]["sls_source"], options["sls"]["dtrace"])
    root = Path(options["sls"]["mount_point"])

    # If this script is setting up the root, ensure it will be torn down in the
    # end.
    if options["magnetosphere"]["setup-root"] == "yes":
        sls.load_modules(Path(options["sls"]["sls_source"]))
        sls.create_root(options)
        teardown_root = True

    if options["sls"]["enabled"] == "yes":
        dtrace_process = mp.Process(target=execute_dtrace,
                                    args=(str(dscript),))
        dtrace_process.start()

    create_paths(root)
    generate_config_file(root, options["redis-server"])

    redis_process = mp.Process(
        target=execute_redis, args=(redis, conf, str(root)))
    redis_process.start()

    if options["sls"]["enabled"] == "yes":
        print("Checkpointing {} with period {}".format(
            redis_process.pid, options["sls"]["PERIOD"]))
        sls.checkpoint(redis_process.pid, options["sls"])

    # Wait for a signal to kill us.
    while True:
        time.sleep(1)


if __name__ == "__main__":
    redis_server()
