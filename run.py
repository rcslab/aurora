#!/usr/bin/env python3

import configargparse
import subprocess
import os
import pwd
import threading
from enum import Enum
from os import path
from os import listdir
from os.path import isfile, join

# Captured output can be disabled (None), capture a single command in which only one file is needed
# or multiple commands in case a directory is needed to store the output
class CapturedOut(Enum):
    NONE = 0
    SINGLE = 1
    MULTI = 2

tests = []

def help_msg(options):
    parser.print_help()

def set_defaults(p):
    p.add('-c', '--config', required=False, is_config_file=True, help='Path to config')
    p.add('--disks', metavar='d1,d2,etc', required=True, action="append", help='Comma seperated values of disk devices')
    p.add('--stripe', required=True, help='Size of stripe of geom layer in bytes')
    p.add('--slsmodule',required=True, metavar='sls', help='Path to sls module')
    p.add('--slosmodule',required=True, metavar='slos', help='Path to slos module')
    p.add('--newfs',required=True, metavar='newfs', help='Path to newfs tool')
    p.add('--mountdir', required=True, metavar='md', help='Directory to mount onto')
    p.add('--stripename', required=True, metavar='n', help='name of stripe device')
    p.add('--checkps', required=True, metavar='cps', help="number of checkpoints per second")
    p.add_argument('--withgstat', required=False, action='store_true', help="Capture Gstat")

parser = configargparse.ArgParser(add_help=True)
parser.set_defaults(func=help_msg)
subparser = parser.add_subparsers(parser_class=configargparse.ArgParser)

def Command(captureOut=CapturedOut.NONE, required=[], help=""):
    def real(func):
        global parser
        def wrapper(*args, **kwargs):
            func(*args, **kwargs)

        name = func.__name__
        h = help
        
        if len(required) > 0 and h == "":
            raise Exception("Please add a help message for the command %s" % name)
        elif h == "":
            h = "No extra arguments required"

        p = subparser.add_parser(name, help=h, default_config_files=['sls.conf'])
        set_defaults(p)

        if captureOut == CapturedOut.SINGLE:
            p.add('-o', required=False, metavar='f.out', help='File to capture command output')
        elif captureOut == CapturedOut.MULTI:
            p.add('-o', required=False, metavar='dirout', help='Directory to capture command output')

        for x in required:
            p.add_argument(x)
        p.set_defaults(func=func)

        return wrapper
    return real

def runcmd(lst, fail_okay=False):
    if fail_okay:
        ret = subprocess.run(lst)
        return ret.returncode
    else:
        subprocess.run(lst).check_returncode()
        return 0


def geom_init(options, disks, stripe):
    create = ["gstripe", "create", "-s", stripe, "-v", options.stripename]
    create.extend(disks)
    destroy_geom = ["gstripe", "destroy", options.stripename]
    runcmd(destroy_geom, fail_okay=True)
    runcmd(create)
    runcmd(destroy_geom)
    runcmd(create)

def kldload(path):
    kldl = ["kldload", path]
    runcmd(kldl, fail_okay=True)

def kldunload(path):
    kldl = ["kldunload", path]
    runcmd(kldl, fail_okay=True)

def newfs(options):
    newf = [options.newfs, "/dev/stripe/{}".format(options.stripename)]
    runcmd(newf)

def mount(options):
    cmd = ["mount", "-t", "slsfs", "/dev/stripe/{}".format(options.stripename), options.mountdir]
    runcmd(cmd)

def umount(path):
    cmd = ["umount", path]
    runcmd(cmd, fail_okay=True)

def init(options):
    geom_init(options, options.disks, options.stripe)
    kldload(options.slosmodule)
    #kldload(options.slsmodule)
    newfs(options)
    mount(options)

def uninit(options):
    umount(options.mountdir)
    kldunload("slos.ko")
    #kldunload("sls.ko")
    destroy_geom = ["gstripe", "destroy", options.stripename]
    runcmd(destroy_geom, fail_okay=True)

def loaded(options):
    return path.exists("/dev/stripe/{}".format(options.stripename))

def get_num_snaps(options):
    cmd = ["./tools/fsdb/fsdb", "-s", "/dev/stripe/{}".format(options.stripename)]
    result = subprocess.run(cmd, stdout=subprocess.PIPE)
    return int(result.stdout.decode('utf-8'))

def gstat(name, timeout, path):
    cmd = ["timeout", str(timeout), "gstat", "-C", "-f", name]
    out = open(path, "w+");
    subprocess.run(cmd, stdout=out)
    out.close()

def startgstat(name, timeout, path):
    x = threading.Thread(target=gstat, args=(name, timeout, path,))
    x.start()
    return x

def runbench(options, path, output):
    cmd = ["filebench", "-f", path]
    gthread = None
    if output != "":
            stdout = open(output, 'w+')
    else:
            stdout = None 

    snap = get_num_snaps(options);

    if (options.withgstat and output != ""):
            path = "{}.gstat.csv".format(output)
            gthread = startgstat(options.stripename, 35, path)

    subprocess.run(cmd, stdout=stdout)
    if (output != ""):
        stdout.close()
        c = "benchmarks/fb-post.sh"
        cmd = [c, output]
        subprocess.run(cmd)
        stdout = open(output, 'a+')
        snap = get_num_snaps(options) - snap;
        stdout.write(str(snap))
        stdout.close()
        if (gthread):
            gthread.join(timeout=5)
    else:
        snap = get_num_snaps(options) - snap;
        print("CHECKPOINTS COMPLETED {}".format(str(snap)))
    
@Command()
def load(options):
    if loaded(options):
        print("Already loaded. Unload first to reload")
        return
    else:
        init(options)
        print("Loaded..")

@Command()
def unload(options):
    uninit(options)
    print("Unloaded..")

@Command(captureOut=CapturedOut.SINGLE, required=["script"], help="filebench script as extra arg")
def benchmark(options):
    if loaded(options):
        print("Already loaded. Unload first to runbenchmark")
        return
    load(options)
    outpath = ""
    if options.o is not None:
        outpath = options.o;

    checkps(options.checkps)
    runbench(options, options.script, outpath)
    umount(options.mountdir)
    #unload(options)

def checkps(num):
    sysctl = ["sysctl", "aurora_slos.checkps={}".format(num)]
    subprocess.run(sysctl);


@Command(required=["dir"], captureOut=CapturedOut.MULTI,
        help="script directory as extra arg")
def allbenchmarks(options):
    if loaded(options):
        print("Already loaded. Unload first to runbenchmark")
    files = [f for f in listdir(options.dir) if isfile(join(options.dir, f))]

    for i, file in enumerate(files):
        print("======= Running %s ======" % file)
        print("======= [%s of %s] ======" % (i + 1, len(files)))
        fullpath = options.dir + "/" + file
        output = ""
        if options.o:
            output= options.o + "/" + file + ".out"

        load(options)
        checkps(options.checkps)
        runbench(options, fullpath, output)
        unload(options)

@Command(required=["script", "min", "max", "steps"], captureOut=CapturedOut.MULTI,
        help="Time series")
def series(options):
    max = int(options.max)
    min = int(options.min)
    stepsize = int((max - min) / int(options.steps))

    if loaded(options):
        print("Already loaded. Unload first to runbenchmark")
        return

    for x in range(min, max + 1, stepsize):
        print("======= Running Step %s ======" % x)
        sysctl = ["sysctl", "aurora_slos.checkps={}".format(x)]
        output = ""
        if options.o:
            output= "{}/{}.out".format(options.o, x)

        load(options)
        checkps(x)
        runbench(options, options.script, output)
        unload(options)
         

@Command(required=["script", "min", "max", "steps"], 
        captureOut=CapturedOut.MULTI,
        help="Time series")
def allseries(options):
    outdir = options.o
    dir = options.script
    files = [f for f in listdir(options.script) if isfile(join(options.script, f))]
    for file in files:
        if (outdir != ""):
            path = "{}/{}".format(outdir, file)
            try:
                os.mkdir(path)
            except:
                pass
            options.o = path
        options.script = "{}/{}".format(dir, file)
        print("======= Running File %s ======" % file)
        series(options)
    
def main():
    global parser
    global subparser
    global com

    options = parser.parse_args()
    options.func(options)

if __name__ == "__main__":
    main()

import os
