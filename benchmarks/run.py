#!/usr/bin/env python3
import configargparse
import subprocess
import os
import pwd
import threading
import shutil
import errno
import select
import urllib
import json
import time
import http.server
import socketserver
import sys
import random
from enum import Enum
from os import path
from os import listdir
from os.path import isfile, join
from pwd import getpwnam
from pathlib import Path

# Needed for benchmarking Firefox
from selenium.webdriver import Firefox, FirefoxProfile
from selenium.webdriver.common.by import By
from selenium.webdriver.common.keys import Keys
from selenium.webdriver.common.desired_capabilities import DesiredCapabilities
from selenium.webdriver.firefox.options import Options
from selenium.webdriver.support import expected_conditions as expected
from selenium.webdriver.support.wait import WebDriverWait

# Captured output can be disabled (None), capture a single command in which only one file is needed
# or multiple commands in case a directory is needed to store the output
class CapturedOut(Enum):
    NONE = 0
    SINGLE = 1
    MULTI = 2

# ====== PARSER CONFIGURATION ======

# Set the arguments for mounting a filesystem for benchmarking/use
def set_defaults_mount(p):
    p.add('--disks', metavar='d1,d2,etc', required=True, action="append",
        help='Comma seperated values of disk devices')
    p.add('--stripe', required=True, help='Size of stripe of geom layer in bytes')
    p.add('--type', required=True, metavar='n',
        help='Type of filesystem to benchmark (or memory)',
        choices=['slos','zfs','ffs', 'memory'])
    p.add('--mountdir', required=True, metavar='md',
        help='Directory to mount onto')
    p.add('--stripename', required=True, metavar='n',
        help='name of stripe device')

# Set the defaults for the SLS module
def set_defaults_sls(p):
    p.add('--slsperiod', required=True, metavar='slsperiod',
        help="SLS checkpointing period in milliseconds (0 for one checkpoint)")
    p.add('--oid', required=True, metavar='oid',
        help="SLS partition OID")
    p.add('--delta', default=False, required=False, metavar='delta',
        help="SLS delta checkpointing")
    p.add('--recursive', default=True, required=False, metavar='recursive',
        help="SLS checkpoints all descendants of processes")
    p.add('--slsctl',required=True, metavar='slsctl',
            help='Path to the slsctl tool')

# Set the defaults for the SLOS module
def set_defaults_slos(p):
    p.add('--checksum', default=False, action="store_true",
        help="Checksumming on")
    p.add('--compress', default=False, action="store_true",
        help="Turn on compress")
    p.add_argument('--withgstat', required=False, action='store_true',
        help="Capture gstat")
    p.add('--checkpointtime', required=True, metavar='checkpointtime',
        help="Number of ms between SLOS checkpoints")

def set_defaults_bench(p):
    p.add('--benchaddr', required=False, metavar='benchaddr', help="Address on which the benchmark server runs")
    p.add('--benchport', required=False, metavar='benchport', help="Address on which the benchmark port runs")
    p.add('--sshaddr', required=False, metavar='sshaddr', help="Address of benchmarking client")
    p.add('--sshport', required=False, metavar='sshport', help="Port of benchmarking client")
    p.add('--sshkey', required=True, metavar='sshkey', help='Key used for sshing into remotes')
    p.add('--user',required=True, metavar='user',
            help='Remote user name for sshing into for benchmarks')

def set_defaults(p):
    p.add('-c', '--config', required=False, is_config_file=True,
        help='Path to config')
    p.add('--slsmodule',required=True, metavar='sls',
            help='Path to sls module')
    p.add('--slosmodule',required=True, metavar='slos', help='Path to slos module')
    p.add('--newfs',required=True, metavar='newfs', help='Path to newfs tool')
    p.add('--runs', default=1, type=int, required=False, help="Number of runs")
    p.add('--nounload', default=False, action="store_true", required=False, help="Unload after benchmark")
    set_defaults_mount(p)
    set_defaults_sls(p)
    set_defaults_slos(p)
    set_defaults_bench(p)

# Wrapper for print_help() that drops all arguments
def help_msg(options):
    parser.print_help()

# Define the parsers.
parser = configargparse.ArgParser(add_help=True)
parser.set_defaults(func=help_msg)
subparser = parser.add_subparsers(parser_class=configargparse.ArgParser)

# Build a new command for running a benchmark.
def Command(captureOut=CapturedOut.NONE, required=[], help="", add_args=[]):
    def real(func):
        # A global parser imported by the module
        global parser
        global subparser

        # Unpacks the arguments to the function
        def wrapper(*args, **kwargs):
            func(*args, **kwargs)

        name = func.__name__
        h = help

        if len(required) > 0 and h == "":
            raise Exception("Please add a help message for the command {}" % name)
        elif h == "":
            h = "No extra arguments required"

        # New parser just for this command
        p = subparser.add_parser(name, help=h,
                default_config_files=['benchmarks/sls.conf'])
        set_defaults(p)

        # Add default names for the output file/directory if needed
        if captureOut == CapturedOut.SINGLE:
            p.add('-o', required=False, metavar='f.out', help='File to capture command output')
        elif captureOut == CapturedOut.MULTI:
            p.add('-o', required=False, metavar='dirout', help='Directory to capture command output')

        # Add any more configuration options needed.
        for x in required:
            p.add_argument(x)
        for x in add_args:
            p.add(*x[0], **x[1])

        # The function to be actually executed
        p.set_defaults(func=func)

        return wrapper
    return real

# ====== BASIC BASH COMMANDS ======

# Construct an SSH command for logging into a remote.
def sshcmd(options):
    return ["ssh", "-i", options.sshkey, "-p", options.sshport,
        "{}@{}".format(options.user, options.sshaddr)]

# Run a bash command
def bashcmd(lst, fail_okay=False):
    if fail_okay:
        # Propagate the return code upwards
        ret = subprocess.run(lst)
        return ret.returncode
    else:
        # Throw an exception if there was an error
        subprocess.run(lst).check_returncode()
        return 0

def ycsbcmd(options, cmd, dbname):
    return ["{}/{}".format(options.ycsb, "bin/ycsb.sh"), cmd, "basic", "-P",
            "{}/workloads/{}".format(options.ycsb, options.workload), "-p",
            "{}.host={}".format(dbname, options.benchaddr),
            "-p", "{}.port={}".format(dbname, options.benchport),
            "-p", "recordcount={}".format(options.recordcount)]

# Do a sysctl into the system
def sysctl(module, key, value):
    if value is None:
        return subprocess.run(["sysctl", "-n", "{}.{}".format(module, key)],
                check=True,
                stdout=subprocess.PIPE).stdout.decode('UTF-8').rstrip()
    else:
        bashcmd(["sysctl", "{}.{}={}".format(module, key, value)])
        return None


def sysctl_slos(key,value=None):
    return sysctl("aurora_slos", key, value)

def sysctl_sls(key,value=None):
    return sysctl("aurora", key, value)

def kldload(path):
    kldl = ["kldload", path]
    return bashcmd(kldl, fail_okay=True)

def kldunload(path):
    kldl = ["kldunload", path]
    return bashcmd(kldl, fail_okay=True)

# Create the full path of the disk by prefixing its name.
def prefixdisk(options):
    # Different prefixes for striped and non-striped disks
    if len(options.disks) == 1:
        return "/dev/{}".format(options.stripename)
    else:
        return  "/dev/stripe/{}".format(options.stripename)

def mount(options):
    # Different prefixes for striped and non-striped disks
    path = prefixdisk(options)

    if (options.type in ["slos", "memory"]):
        cmd = ["mount", "-t", "slsfs", path, options.mountdir]
    elif (options.type == "zfs"):
        cmd = ["zfs", "set", "mountpoint={}".format(options.mountdir), "{}{}".format(options.stripename, options.mountdir)]
    elif (options.type == "ffs"):
        cmd = ["mount", path, options.mountdir]
    bashcmd(cmd)
    os.chmod(options.mountdir, 0o777)

def umount(options):
    if (options.type == "zfs"):
        cmd = ["zfs", "destroy", "-r", "{}{}".format(options.stripename, options.mountdir)]
        bashcmd(cmd)
        cmd = ["zpool", "destroy", options.stripename]
        bashcmd(cmd)
    else:
        cmd = ["umount", options.mountdir]
        bashcmd(cmd, fail_okay=True)

# Return the disk we'll use for the SLOS. If it's a geom stripe, create it.
def geom_init(options, disks, stripe):
    if (options.type != "zfs"):
        # Gstripe does not work with 1 disk
        if len(disks) == 1:
            options.stripename=disks[0]
            return

        create = ["gstripe", "create", "-s", stripe, "-v", options.stripename]
        create.extend(disks)
        destroy_geom = ["gstripe", "destroy", options.stripename]
        # XXX Error checking? We have fail_okay
        bashcmd(destroy_geom, fail_okay=True)
        bashcmd(create, fail_okay=True)
        bashcmd(destroy_geom, fail_okay=True)
        if (bashcmd(create, fail_okay=True)):
            print("\nERROR: Problem with loading gstripe\n")
            unload(options)
            exit (1)

# Create a new filesystem. This can be a regular filesystem or a SLOS
def newfs(options):
    path = prefixdisk(options)

    if (options.type in ["slos", "memory"]):
        newf = [options.newfs, path]
        bashcmd(newf)
    elif (options.type == "ffs"):
        newf = ["newfs", "-j", "-S", "4096", "-b", options.stripe, path]
        bashcmd(newf)
    elif (options.type == "zfs"):
        zpool = ["zpool", "create", options.stripename]
        zpool.extend(options.disks)
        bashcmd(zpool)

        if (options.compress):
            zpool = ["zfs", "set", "compression=lz4", options.stripename]
            bashcmd(zpool)

        if (options.checksum):
            zpool = ["zfs", "set", "checksum=on", options.stripename]
            bashcmd(zpool)
        else:
            zpool = ["zfs", "set", "checksum=off", options.stripename]
            bashcmd(zpool)

        zpool = ["zfs", "set", "recordsize={}".format(options.stripe),
                options.stripename]
        bashcmd(zpool)

        zpool = ["zfs", "create", "{}{}".format(options.stripename, options.mountdir)]
        bashcmd(zpool)

    else:
        raise Exception("Invalid backend {} specified".format(options.type))

# Set up all modules and filesystems.
def module_init(options):
    if (options.type != "zfs"):
        geom_init(options, options.disks, options.stripe)
    if (options.type in ["slos", "memory"]):
        if kldload(options.slosmodule):
            raise Exception("SLOS module already loaded")
        if kldload(options.slsmodule):
            raise Exception("SLS module already loaded")
        sysctl_slos("checkpointtime", options.checkpointtime)

    newfs(options)
    mount(options)

# Clean up for the work done in module_init().
def module_fini(options):
    umount(options)
    if (options.type in ["slos", "memory"]):
        kldunload("slos.ko")
        kldunload("sls.ko")
    if (options.type != "zfs"):
        destroy_geom = ["gstripe", "destroy", options.stripename]
        bashcmd(destroy_geom, fail_okay=True)

# ===== SLOS BENCHMARKING COMMANDS =====

# Check if the stripe already exists
def stripe_loaded(options):
    return path.exists("/dev/stripe/{}".format(options.stripename))

def get_num_snaps(options):
    if (options.type == "slos"):
        cmd = ["../tools/fsdb/fsdb", "-s", "/dev/stripe/{}".format(options.stripename)]
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
            gthread = startgstat(options.stripename, 40, path)

    subprocess.run(cmd, stdout=stdout)
    if (output != ""):
        stdout.close()
        c = "fb-post.sh"
        cmd = [c, output]
        subprocess.run(cmd)
        stdout = open(output, 'a+')
        snap = get_num_snaps(options) - snap;
        stdout.write(str(snap))
        stdout.close()
        if (gthread):
            gthread.join(timeout=25)
    else:
        snap = get_num_snaps(options) - snap;
        print("CHECKPOINTS COMPLETED {}".format(str(snap)))


@Command()
def load(options):
    # XXX Why is having a stripe equivalent to having everything loaded?
    if stripe_loaded(options):
        print("Already loaded. Unload first to reload")
        exit (1)
    else:
        module_init(options)
        print("Loaded..")

@Command()
def unload(options):
    if (options.nounload):
        print("Unloading not permitted")
        return

    module_fini(options)
    print("Unloaded..")

@Command(captureOut=CapturedOut.SINGLE, required=["script"], help="filebench script as extra arg")
def benchmark(options):
    if stripe_loaded(options):
        print("Already loaded. Unload first to runbenchmark")
        return
    load(options)
    outpath = ""
    if options.o is not None:
        outpath = options.o;

    runbench(options, options.script, outpath)
    unload(options)

@Command(required=["dir"], captureOut=CapturedOut.MULTI,
        help="script directory as extra arg")
def allbenchmarks(options):
    if stripe_loaded(options):
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
            runbench(options, fullpath, output)
            unload(options)

@Command(required=["script", "min", "max", "steps"], captureOut=CapturedOut.MULTI,
        help="Time series")
def series(options):
    max = int(options.max)
    min = int(options.min)

    if stripe_loaded(options):
        print("Already loaded. Unload first to runbenchmark")
        return

    for x in range(0,  int(options.steps)):
        value = min + (((max - min) * x) // (int(options.steps) - 1))
        print("======= Running Step %s ======" % value)
        options.checkps = value
        if options.o:
            output= "{}/{}.out".format(options.o, value)

        load(options)
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

# ===== SLS BENCHMARKS =====

def parse_wrkoutput(wrkoutput):
    # The patterns we are looking for
    cols = lambda l : l[1:]
    last = lambda l : [l[-1]]
    reqtime = lambda l: [l[0], l[3][:-1], l[4]]

    match = {
            "Latency" : (cols, ["lavg", "lstdev", "lmax", "lstdev-pm"]),
            "Req/Sec" : (cols, ["ravg", "rstdev", "rmax", "rstdev-pm"]),
            "requests in": (reqtime, ["requests", "time", "read"]),
            # XXX This is going to break if we actually serve files.
            "Non-2xx": (last, ["non2xx"]),
            "Requests/sec": (last, ["requestps"]),
            "Transfer/sec": (last, ["transferps"]),
    }

    output = []
    for k,v in match.items():
        output.extend(v[1])

    print(",".join(output) + "\n")

    output = []
    for line in wrkoutput.splitlines():
        for k,v in match.items():
            if k in line:
                vals = [x.strip() for x in line.split()]
                output.extend(v[0](vals))
    print(",".join(output))



# Get the PID of the main process we're checkpointing. Benchmarks often have
# multiple processes, so we need to get the root of its process tree, assuming
# it exists. Note that we can only do this if we assume non-random PIDs, and
# even then PID allocation must not wrap around.
def pid_main(benchname):
    cmd = ["pidof", benchname]
    output = subprocess.run(cmd, check=True,
            stdout=subprocess.PIPE).stdout.decode('UTF-8')
    pids = list(map(int, output.strip().split()))
    pids.sort()
    return pids[0]


# Create directories in the SLOS to be used by the benchmarks. By appropriately
# modifying the configuration files, and building a directory tree similar to
# that present at the root, we can ensure benchmarks only create files in the
# SLOS, and are therefore definitely checkpointable.
def make_slsdirs(options, benchmark):
    folders = ["data", "log", "log/" + benchmark, "logs",
        "tmp", "var", "var/cache", "var/cache/" + benchmark,
        "var/run", "var/run/" + benchmark, benchmark]
    for folder in folders:
        path = "{}/{}".format(options.mountdir, folder)
        try:
            Path(path).mkdir(exist_ok=True)
        except OSError as err:
            # It's fine if the file already exists
            if err.errno != errno.EEXIST:
                print("Error {} creating folder {}".format(err, folder))
                raise


# Insert a series of PIDs into a partition and start checkpointing them.
def slsckpt(options, pidlist):

    print("Starting Aurora Checkpointer on {}".format(str(pidlist)))

    # If period is 0 we do not put the PIDs in the SLS.
    if options.slsperiod == 0:
        return

    if not (options.type in ["slos", "memory"]):
        raise Exception("Invalid SLS backend {}".format(options.type))

    cmd = [options.slsctl, "partadd",
            "-o", options.oid, "-b", options.type,
            "-t", str(options.slsperiod)]
    if options.delta:
        cmd.append("-d")

    bashcmd(cmd)

    for pid in pidlist:
        cmd = [options.slsctl, "attach", "-o", options.oid, "-p", str(pid)]
    bashcmd(cmd)

    cmd = [options.slsctl, "checkpoint", "-o", options.oid]
    if options.recursive:
        cmd.append("-r")

    bashcmd(cmd)
    print("Started Aurora Checkpointer on {}".format(str(pidlist)))

# Generate a configuration from a template
def generate_conf(options, inputconf, outputconfig):
    # The templated variables.
    replace_list = [
            ["SLS_MOUNT", options.mountdir],
            ["SLS_SERVER_URL", options.benchaddr],
            ["SLS_SERVER_PORT", options.benchport]
    ]

    # Search and replace all template strings in the files. The way we specify
    # the path makes it necessary for us to be in the base SLS directory when
    # running the script, we need to change it in the future.
    with open(inputconf, 'r') as templatefile:
        with open(outputconfig, 'w+') as conffile:
            for line in templatefile:
                for x in replace_list:
                    line = line.replace(x[0], x[1])
                conffile.write(line)

# Create a configuration file for the web server so that it only uses files in
# the SLS. This is needed to be able to checkpoint filesystem state.
def webserver_createconf(options, inputconf, outputconf, srvconf):

    # Link the original directory used by the server into the SLOS. Done so we
    # can control the working directory of the server.
    os.symlink(srvconf, "{}/{}".format(options.mountdir, options.server))

    # Create the folders the web server expects to find at specific places.
    # We create the lighttpd/nginx configuration from a template we have
    # already constructed, so the file already includes the modified folder
    # paths we are creating here.
    make_slsdirs(options, options.server)

    # Generate the configuration file
    generate_conf(options, inputconf, outputconf)


# Start the lighttpd server
def lighttpd_setup(options, inputconf, outputconf, srvconf):
    # Get the current difrectory, so that we can switch back to it later.
    pwd = os.getcwd()

    # Create the configuration
    webserver_createconf(options, inputconf, outputconf, srvconf)
    os.chdir(options.mountdir)

    # Start lighttpd using the new config file.
    cmd = [options.lighttpd, '-f', outputconf]
    bashcmd(cmd)

    os.chdir(pwd)

    # Get the PID of the newly created server.
    return pid_main("lighttpd")

# Start the lighttpd server
def nginx_setup(options, inputconf, outputconf, srvconf):
    # Get the current difrectory, so that we can switch back to it later.
    pwd = os.getcwd()

    # Create the configuration
    webserver_createconf(options, inputconf, outputconf, srvconf)
    os.chdir(options.mountdir)

    # Start nginx using the new config file.
    cmd = [options.nginx, '-c', outputconf]
    bashcmd(cmd)

    os.chdir(pwd)

    # Get the PID of the newly created server. There is a master nginx process
    # and a set of worker processes. Assuming non-random PIDs, the smallest PID
    # is the original process, so we get the output of pidof and parse it to
    # select the smallest possible process.
    return pid_main("nginx")

def webserver_setup(options):
    # Apart from selecting the right setup function to call, set the path of
    # the configuration to be used and the output configuration. No need for
    # special OS path join methods, since we can only possibly run on FreeBSD
    # anyway.
    if options.server == "nginx":
        return nginx_setup(options,
                inputconf="benchmarks/nginx.conf",
                outputconf="{}/{}".format(options.mountdir,
                "nginx/nginx.conf"),
                srvconf=options.nginxconfdir
                )
    elif options.server == "lighttpd":
        return lighttpd_setup(options,
                inputconf="benchmarks/lighttpd.conf",
                outputconf="{}/{}".format(options.mountdir,
                "lighttpd/lighttpd.conf"),
                srvconf=options.lighttpdconfdir
                )
    else:
        raise Exception("Invalid server {}".format(options.server))


# Command for spinning up a webserver and taking numbers
@Command(required=[],
    captureOut=CapturedOut.MULTI,
    add_args=[
            [
                # Default locations of the binaries and config files
                ['--lighttpd'],
                {
                    "action" : "store",
                    "default" : "/usr/local/sbin/lighttpd",
                    "help" : "Location of lighttpd"
                }
            ],
            [
                ['--nginx'],
                {
                    "action" : "store",
                    "default" : "/usr/local/sbin/nginx",
                    "help" : "Location of nginx"
                }
            ],
            [
                ['--lighttpdconfdir'],
                {
                    "action" : "store",
                    "default" : "/usr/local/etc/lighttpd",
                    "help" : "Location of lighttpd config dir",
                }
            ],
            [
                ['--nginxconfdir'],
                {
                    "action" : "store",
                    "default" : "/usr/local/etc/nginx",
                    "help" : "Location of nginx config dir",
                }
            ],
            [
                ['--server'],
                {
                    "action" : "store",
                    "default" : "nginx",
                    "help" : "Standard web server to use",
                }
            ],
            [
                ['--threads'],
                {
                    "action" : "store",
                    "default" : "10",
                    "help" : "Number of client threads to use"
                }
            ],
            [
                ['--connections'],
                {
                    "action" : "store",
                    "default" : "50",
                    "help" : "Number of connections used by wrk",
                }
            ],
            [
                ['--time'],
                {
                    "action" : "store",
                    "default" : "10",
                    "help" : "Duration of the benchmark in seconds",
                }
            ],
        ],
        help="Run a web server sls workload")
def webserver(options):
    # XXX Replace with module_init?
    load(options)

    # Start lighttpd
    pid = webserver_setup(options)

    # Begin the SLS
    if (options.type in ["slos", "memory"]):
        slsckpt(options, [pid])

    # Run the benchmark.
    ssh = sshcmd(options)
    wrk = ["wrk", "-d", str(options.time), "-t", str(options.threads), \
            "-c", str(options.connections),
            "http://{}:{}".format(options.benchaddr, options.benchport)]
    wrkoutput = subprocess.run(ssh + wrk,
            stdout=subprocess.PIPE).stdout.decode('UTF-8')
    parse_wrkoutput(wrkoutput)

    # Kill the server
    cmd = ['pkill', '-9', options.server]
    bashcmd(cmd)
    print("{} checkpoints (expected around {})".format(
        sysctl_sls("ckpt_done"),
        (1000 / int(options.slsperiod)) * int(options.time)))

    # XXX Replace with module_fini?
    unload(options)

def redis_setup(options):
    # Get the current directory, so that we can switch back to it later.
    pwd = os.getcwd()

    # XXX Make them defaults with the Redis command somehow
    inputconfig = "benchmarks/redis.conf"
    outputconfig = "{}/{}".format(options.mountdir, "redis.conf")
    generate_conf(options, inputconfig, outputconfig)

    # Create the configuration
    os.chdir(options.mountdir)

    make_slsdirs(options, "redis")

    # Start lighttpd using the new config file.
    cmd = [options.redis, outputconfig]
    bashcmd(cmd)

    os.chdir(pwd)

    # Get the PID of the newly created server. There is a master nginx process
    # and a set of worker processes. Assuming non-random PIDs, the smallest PID
    # is the original process, so we get the output of pidof and parse it to
    # select the smallest possible process.
    return pid_main("redis-server")

def memcached_setup(options):
    # Get the current directory, so that we can switch back to it later.
    pwd = os.getcwd()

    make_slsdirs(options, "memcached")

    # Create the directory for the PID file
    cmd = ["memcached", "-u", options.user, "-l", options.benchaddr,
            "-p", options.benchport, "-P", "{}/{}".format(options.mountdir,
            "memcached.pid"), "-d"]
    bashcmd(cmd)

    # Switch back to the original directory.
    os.chdir(pwd)
 
    return pid_main("memcached")

# Command for spinning up a webserver and taking numbers
@Command(required=[],
    captureOut=CapturedOut.MULTI,
    add_args=[
            [
                # Default locations of the binaries and config files
                ['--redis'],
                {
                    "action" : "store",
                    "default" : "/usr/local/bin/redis-server",
                    "help" : "Location of the Redis server"
                }
            ],
            [
                # Default locations of the binaries and config files
                ['--ycsb'],
                {
                    "action" : "store",
                    "default" : "/home/etsal/ycsb/",
                    "help" : "Location of the YCSB directory"
                }
            ],
            [
                ['--recordcount'],
                {
                    "action" : "store",
                    "default" : str(100 * 1000),
                    "help" : "Number of records to be loaded into the database"
                }

            ],
            [
                ['--workload'],
                {
                    "action" : "store",
                    "default" : "workloada",
                    "help" : "Workload profile to be used with YCSB" }

            ],
            [
                ['--kvstore'],
                {
                    "action" : "store",
                    "default" : "redis",
                    "help" : "The key-value store to be benchmarked",
                }
            ],
            [
                ['--memcached'],
                {
                    "action" : "store",
                    "default" : "/usr/local/bin/memcached",
                    "help" : "Location of the memcached server"
                }
            ],
            [
                ['--memcacheduser'],
                {
                    "action" : "store",
                    "default" : "root",
                    "help" : "User under which memcached runs"
                }
            ],
        ],
        help="Run the Redis server")
def kvstore(options):
    # XXX Replace with module_init?
    load(options)

    # Start lighttpd
    if options.kvstore == "redis":
        pid = redis_setup(options)
    elif options.kvstore == "memcached":
        pid = memcached_setup(options)
    else:
        raise Exception("Invalid options.kvstore {}".format(options.kvstore))

    if pid == None:
        raise Exception("No PID for process")
    print(pid)

    time.sleep(10)

    # Warm it up using YCSB. We can do this locally, but then we would need two
    # version of YCSB - one collocated with the database, and one remote.
    ssh = sshcmd(options)
    cmd = ycsbcmd(options, "load", options.kvstore)
    bashcmd(ssh + cmd)

    # Insert the server into the SLS.
    if (options.type in ["slos", "memory"]):
        slsckpt(options, [pid])

    # SSH into the remote and start the benchmark.
    # XXX Parse it into a form we can use for graphing.
    cmd = ycsbcmd(options, "run", options.kvstore)
    bashcmd(ssh + cmd)

    # Kill the server
    cmd = ['kill', '-9', str(pid)]
    bashcmd(cmd)

    # XXX Measure time
    #print("{} checkpoints (expected around {})".format(
        #sysctl_sls("ckpt_done"),
        #(1000 / int(options.slsperiod)) * sleeptime))

    # Wait for a bit to avoid races # XXX Necessary?
    time.sleep(3)

    # XXX Replace with module_fini?
    unload(options)

def firefox_benchmark(options):
    ffoptions = Options()
    ffoptions.add_argument('-headless')

    profile = FirefoxProfile()
    profile.DEFAULT_PREFERENCES['frozen']['network.http.spdy.enabled.http2'] = False
    profile.DEFAULT_PREFERENCES['frozen']['browser.tabs.remote.autostart'] = False
    profile.DEFAULT_PREFERENCES['frozen']['browser.tabs.remote.autostart.2'] = False
    profile.DEFAULT_PREFERENCES['frozen']['autostarter.privatebrowsing.autostart'] = False

    cap = DesiredCapabilities().FIREFOX
    cap["marionette"] = True

    driver = Firefox(firefox_binary=options.firefox, options=ffoptions,
            firefox_profile=profile, capabilities=cap)
    wait = WebDriverWait(driver, timeout=10000)

    url = "http://{}:{}/{}".format(options.benchaddr, options.benchport, 
            options.firefoxdriver)
    driver.get(url)

    wait.until(lambda driver : "results" in driver.current_url)
    values = urllib.parse.unquote(driver.current_url.split('?')[1])
    vals = json.loads(values)
    runtime = 0
    for key, v in vals.items():
        if (key != "v"):
            runtime += sum(list(map(int, v)))
    print("Time: {} ms".format(runtime))

    driver.close()
    driver.quit()

# Command for spinning up a webserver and taking numbers
@Command(required=[],
    captureOut=CapturedOut.MULTI,
    add_args=[
            [
                # Default locations of the binaries and config files
                ['--firefox'],
                {
                    "action" : "store",
                    "default" : "/usr/local/bin/firefox",
                    "help" : "Location of the Firefox binary"
                }
            ],
            [
                # Default path of the benchmark in the server
                ['--firefoxdriver'],
                {
                    "action" : "store",
                    "default" : "/kraken-1.1/driver.html",
                    "help" : "URL of the driver of the benchmark"
                }
            ],
        ],
        help="Run the Firefox JS benchmark")
def firefox(options):
    # XXX Replace with module_init?
    load(options)

    random.seed()

    # Choose a random port every time. Created sockets linger, so if want to be
    # able to run the benchmark multiple times we need to use a different port
    # each time.
    options.benchaddr="localhost"
    options.benchport=str(random.randrange(8000, 16000))

    # Create the server, serve forever. This is the server that _serves_ the
    # benchmarks, not the benchmark that executes the JS and gets checkpointed.
    serverpid = os.fork()
    if serverpid == 0:
        os.chdir("/root/sls-bench/firefox/hosted")
        handler = http.server.SimpleHTTPRequestHandler
        httpd = socketserver.TCPServer((options.benchaddr,
            int(options.benchport)), handler)
        httpd.serve_forever()

    # Spawn the new process. This is the process that ultimately spawns the
    # benchmark driver that gets checkpointed.
    benchpid = os.fork()
    if benchpid == 0:
        os.chdir(options.mountdir)
        firefox_benchmark(options)
        sys.exit()

    time.sleep(3)

    # Begin the SLS.
    if (options.type in ["slos", "memory"]):
        slsckpt(options, [benchpid])

    # Wait for the benchmark to be done
    os.waitpid(benchpid, 0)

    # Kill the server and the driver
    cmd = ['kill', '-15', str(serverpid)]
    bashcmd(cmd, fail_okay=True)

    # XXX Statistics message with the config
    #print("{} checkpoints (expected around {})".format(
    #    sysctl_sls("ckpt_done"),
    #    (1000 / int(options.slsperiod)) * sleeptime))

    # XXX Replace with module_fini?
    unload(options)

@Command(required=["--server"],
        captureOut=CapturedOut.MULTI,
        # XXX Find a way to not repeat these, is it possible though with the
        # way we're using the parser and building a global object?
        add_args=[
            [
                # Default locations of the binaries and config files
                ['--lighttpd'],
                {
                    "action" : "store",
                    "default" : "/usr/local/sbin/lighttpd",
                    "help" : "Location of lighttpd"
                }
            ],
            [
                ['--nginx'],
                {
                    "action" : "store",
                    "default" : "/usr/local/sbin/nginx",
                    "help" : "Location of nginx"
                }
            ],
            [
                ['--lighttpdconfdir'],
                {
                    "action" : "store",
                    "default" : "/usr/local/etc/lighttpd",
                    "help" : "Location of lighttpd config dir",
                }
            ],
            [
                ['--nginxconfdir'],
                {
                    "action" : "store",
                    "default" : "/usr/local/etc/nginx",
                    "help" : "Location of nginx config dir",
                }
            ],
            [
                ['--threads'],
                {
                    "action" : "store",
                    "default" : "10",
                    "help" : "Number of client threads to use"
                }
            ],
            [
                ['--connections'],
                {
                    "action" : "store",
                    "default" : "50",
                    "help" : "Number of connections used by wrk",
                }
            ],
            [
                ['--time'],
                {
                    "action" : "store",
                    "default" : "10",
                    "help" : "Duration of the benchmark in seconds",
                }
            ],
        ],
        help="Run the webserver benchmark multiple times")
def webbench(options):
    for interval in [10, 100]:
        for slsperiod in range(interval, interval * 10 + 1, interval):
            options.slsperiod=slsperiod
            # XXX How to call the decorator before the thing
            webserver(options)

def main():
    global parser
    global subparser
    global com

    options = parser.parse_args()
    options.func(options)

if __name__ == "__main__":
    main()
