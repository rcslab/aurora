#!/usr/bin/env python3

import configargparse
import subprocess
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

destroy_geom = ["gstripe", "destroy", "st0"]
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


def geom_init(disks, stripe):
    create = ["gstripe", "create", "-s", stripe, "-v", "st0"]
    create.extend(disks)
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

def newfs(path):
    newf = [path, "/dev/stripe/st0"]
    runcmd(newf)

def mount(path):
    cmd = ["mount", "-t", "slsfs", "/dev/stripe/st0", path]
    runcmd(cmd)

def umount(path):
    cmd = ["umount", path]
    runcmd(cmd, fail_okay=True)

def init(options):
    geom_init(options.disks, options.stripe)
    kldload(options.slosmodule)
    kldload(options.slsmodule)
    newfs(options.newfs)
    mount(options.mountdir)

def uninit(options):
    umount(options.mountdir)
    kldunload("slos.ko")
    kldunload("sls.ko")
    runcmd(destroy_geom, fail_okay=True)

def loaded():
    return path.exists("/dev/stripe/st0")

def runbench(path, output):
    cmd = ["filebench", "-f", path]
    if output != "":
            stdout = open(output, 'w')
    else:
            stdout = None 

    subprocess.run(cmd, stdout=stdout)
   
@Command()
def load(options):
    if loaded():
        print("Already loaded. Unload first to reload")
        return
    else:
        init(options)
        print("Loaded..")

@Command()
def unload(options):
    uninit(options)
    print("Unloaded..")

@Command(captureOut=CapturedOut.SINGLE, 
        required=["script"], 
        help="filebench script as extra arg")
def runbenchmark(options):
    if loaded():
        print("Already loaded. Unload first to runbenchmark")
        return
    load(options)
    outpath = ""
    if options.o is not None:
        outpath = options.o;

    runbench(options.script, outpath)
    unload(options)

@Command(required=["dir"], captureOut=CapturedOut.MULTI,
        help="script directory as extra arg")
def runallbenchmarks(options):
    if loaded():
        print("Already loaded. Unload first to runbenchmark")
    print(options)
    files = [f for f in listdir(options.dir) if isfile(join(options.dir, f))]

    for i, file in enumerate(files):
        print("======= Running %s ======" % file)
        print("======= [%s of %s] ======" % (i + 1, len(files)))
        fullpath = options.dir + "/" + file
        output = ""
        if options.o:
            output= options.o + "/" + file + ".out"
        load(options)
        runbench(fullpath, output)
        unload(options)


def main():
    global parser
    global subparser
    global com

    options = parser.parse_args()
    options.func(options)

if __name__ == "__main__":
    main()

