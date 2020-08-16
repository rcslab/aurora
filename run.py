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
    p.add('--type', required=True, metavar='n', help='Type of filesystem to benchmark', choices=['sls','zfs','ffs'])
    p.add('--checkps', required=True, metavar='cps', help="number of checkpoints per second")
    p.add('--compress', default=False, action="store_true", help="Turn on compress")
    p.add('--checksum', default=False, action="store_true", help="Checksumming on")
    p.add('--runs', default=1, type=int, required=False, help="Runs")
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
    if (options.type != "zfs"):
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
    if (options.type == "sls"):
        newf = [options.newfs, "/dev/stripe/{}".format(options.stripename)]
        runcmd(newf)
    elif (options.type == "ffs"):
        newf = ["newfs", "-j", "-S", "4096", "-b", options.stripe, "/dev/stripe/{}".format(options.stripename)]
        runcmd(newf)
    elif (options.type == "zfs"):
        zpool = ["zpool", "create", options.stripename]
        zpool.extend(options.disks)
        runcmd(zpool)

        if (options.compress):
            zpool = ["zfs", "set", "compression=lz4", options.stripename]
            runcmd(zpool)

        if (options.checksum):
            zpool = ["zfs", "set", "checksum=on", options.stripename]
            runcmd(zpool)
        else:
            zpool = ["zfs", "set", "checksum=off", options.stripename]
            runcmd(zpool)

        zpool = ["zfs", "set", "recordsize={}".format(options.stripe), 
                options.stripename]
        runcmd(zpool)

        zpool = ["zfs", "create", "{}{}".format(options.stripename, options.mountdir)]
        runcmd(zpool)

    else:
        exit(1)

def mount(options):
    if (options.type == "sls"):
        cmd = ["mount", "-t", "slsfs", "/dev/stripe/{}".format(options.stripename), options.mountdir]
    elif (options.type == "zfs"):
        cmd = ["zfs", "set", "mountpoint={}".format(options.mountdir), "{}{}".format(options.stripename, options.mountdir)]
    elif (options.type == "ffs"):
        cmd = ["mount", "/dev/stripe/{}".format(options.stripename), options.mountdir]
    else:
        exit(1);
    runcmd(cmd)
   
def umount(options):
    if (options.type == "zfs"):
        cmd = ["zfs", "destroy", "-r", "{}{}".format(options.stripename, options.mountdir)]
        runcmd(cmd)
        cmd = ["zpool", "destroy", options.stripename]
        runcmd(cmd)
    else:
        cmd = ["umount", options.mountdir]
        runcmd(cmd, fail_okay=True)

def init(options):
    if (options.type != "zfs"):
        geom_init(options, options.disks, options.stripe)
    if (options.type == "sls"):
        kldload(options.slosmodule)
        #kldload(options.slsmodule)
    newfs(options)
    mount(options)

def uninit(options):
    umount(options)
    if (options.type == "sls"):
        kldunload("slos.ko")
        #kldunload("sls.ko")
    if (options.type != "zfs"):
        destroy_geom = ["gstripe", "destroy", options.stripename]
        runcmd(destroy_geom, fail_okay=True)

def loaded(options):
    return path.exists("/dev/stripe/{}".format(options.stripename))

def get_num_snaps(options):
    if (options.type == "sls"):
        cmd = ["./tools/fsdb/fsdb", "-s", "/dev/stripe/{}".format(options.stripename)]
        result = subprocess.run(cmd, stdout=subprocess.PIPE)
        return int(result.stdout.decode('utf-8'))
    else:
        return 0

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
            gthread = startgstat(options.stripename, 50, path)

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
    unload(options)

def checkps(num):
    sysctl = ["sysctl", "aurora_slos.checkps={}".format(num)]
    subprocess.run(sysctl);


@Command(required=["dir"], captureOut=CapturedOut.MULTI,
        help="script directory as extra arg")
def allbenchmarks(options):
    if loaded(options):
        print("Already loaded. Unload first to runbenchmark")
    files = [f for f in listdir(options.dir) if isfile(join(options.dir, f))]
    out = options.o
    for x in range(0, int(options.runs)):
        print("===== Run %s ======" % str(x))
        if out:
            outdir = out + "/" + str(x) + "/"
            try:
                os.mkdir(outdir)
                os.chmod(outdir, 0o777)
            except:
                pass
        else:
            outdir = ""
        for i, file in enumerate(files):
            print("======= Running %s ======" % file)
            print("======= [%s of %s] ======" % (i + 1, len(files)))
            fullpath = options.dir + "/" + file
            output = ""
            if outdir:
                output = outdir + "/" + file + ".out"
            load(options)
            checkps(options.checkps)
            runbench(options, fullpath, output)
            unload(options)

@Command(required=["script", "min", "max", "steps"], captureOut=CapturedOut.MULTI,
        help="Time series")
def series(options):
    max = int(options.max)
    min = int(options.min)

    if loaded(options):
        print("Already loaded. Unload first to runbenchmark")
        return

    for x in range(0,  int(options.steps)):
        value = min + (((max - min) * x) // (int(options.steps) - 1))
        print("======= Running Step %s ======" % value)
        sysctl = ["sysctl", "aurora_slos.checkps={}".format(value)]
        output = ""
        if options.o:
            output= "{}/{}.out".format(options.o, value)

        load(options)
        checkps(value)
        runbench(options, options.script, output)
        unload(options)
         

@Command(required=["script", "min", "max", "steps"], 
        captureOut=CapturedOut.MULTI,
        help="Time series")
def allseries(options):
    outputdir = options.o
    dir = options.script
    files = [f for f in listdir(dir) if isfile(join(dir, f))]
    for x in range(0, options.runs):
        print("===== Run %s ======" % str(x))
        if (outputdir != ""):
            outdir = outputdir + "/" + str(x) + "/"
            try:
                os.mkdir(outdir)
                os.chmod(outdir, 0o777)
            except:
                pass
        else:
            outdir = ""
        for file in files:
            if (outdir != ""):
                path = "{}/{}".format(outdir, file)
                try:
                    os.mkdir(path)
                    os.chmod(path, 0o777)
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
