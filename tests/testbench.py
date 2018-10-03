#!/usr/bin/env python

import sys
import os
import time
import subprocess

TIMEOUT = 30.0

CLEAR = "\r\033[K"
RED = "\033[0;31m"
GREEN = "\033[0;32m"
YELLOW = "\033[0;33m"
NORMAL = "\033[0;39m"

FORMAT = "%-32s [ %s%-9s"+ NORMAL + " ]"
TFORMAT = "%-32s [ %s%-9s"+ NORMAL + " ] %-10.6f"

def DeleteFile(name):
    try:
        os.unlink(name)
    except OSError:
        pass


all_tests = []
for f in sorted(os.listdir('tests/')):
    if f.endswith("_test.c"):
        all_tests.append(os.path.basename(f[0:-7]))
    if f.endswith("_test.cc"):
        all_tests.append(os.path.basename(f[0:-8]))
    if f.endswith(".log"):
        DeleteFile(f[0:-4])

tests = [ ]
failed = [ ]
disabled = [ ]

def write(str):
    sys.stdout.write(str)
    sys.stdout.flush()

def CleanTest(name):
    DeleteFile(name + "_test.log")
    DeleteFile(name + "_test.core")

def ReportError(name):
    write(CLEAR)
    write(FORMAT % (name, RED, "Failed") + "\n")
    failed.append(name)

def ReportTimeout(name):
    write(CLEAR)
    write(FORMAT % (name, RED, "Timeout") + "\n")
    failed.append(name)

def ReportDisabled(name):
    write(CLEAR)
    write(FORMAT % (name, YELLOW, "Disabled") + "\n")
    disabled.append(name)

def ReadDisabledList():
    with open('DISABLED') as f:
        disabled_tests = f.read().splitlines()
    autogenerate_list = map(lambda x: x.strip(), disabled_tests)
    autogenerate_list = filter(lambda x:not x.startswith('#'), disabled_tests)
    return disabled_tests

def Run(tool, name, output):
    outfile = open(output, "w+")
    start = time.time()
    t = subprocess.Popen(["../build/tests/" + name + "_test"],
                         stdout=outfile,
                         stderr=outfile)
    while 1:
        t.poll()
        if t.returncode == 0:
            return (time.time() - start)
        if t.returncode != None:
            ReportError(name)
            return None
        if (time.time() - start) > TIMEOUT:
            t.kill()
            ReportTimeout(name)
            return None

def RunTest(name):
    write(FORMAT % (name, NORMAL, "Running"))

    # Normal
    norm_time = Run([], name, name + "_test.log")
    if norm_time is None:
        return

    write(CLEAR)
    write(TFORMAT % (name, GREEN, "Completed", norm_time))
    write("\n")

basedir = os.getcwd()
if (basedir.split('/')[-1] != 'tests'):
    os.chdir('tests')

if len(sys.argv) > 1:
    for t in sys.argv[1:]:
        if all_tests.count(t) == 0:
            print "Test '%s' does not exist!" % (t)
            sys.exit(255)
        tests.append(t)
else:
    tests = all_tests

write("%-32s   %-9s   %-10s\n" %
        ("Test", "Status", "Time"))
write("-------------------------------------------------------\n")
disabled_tests = ReadDisabledList()
for t in tests:
    if t in disabled_tests:
        ReportDisabled(t)
        continue
    CleanTest(t)
    RunTest(t)

if len(failed) != 0:
    print str(len(failed)) + " tests failed"
if len(disabled) != 0:
    print str(len(disabled)) + " tests disabled"

sys.exit(len(failed))

