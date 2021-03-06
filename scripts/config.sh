# Base directory of the SLS tree
SLSDIR="/root/sls/"
# Location of the slsctl utility
SLSCTL="$SLSDIR/tools/slsctl/slsctl"
# Location of the newfs utility
SLSFS="$SLSDIR/tools/newfs_sls/newfs_sls"

# SLS parameters

# Base OID to use
OID="1000"
# Backend to use when checkpointing
BACKEND="slos"
# Checkpointing period in milliseconds
CKPTFREQ="1000"
# Use deltas?
DELTA="no"
# Restore in a stopped state?
RESTSTOP="no"

# Directory in which we place our output (configuration and results)
OUTDIR="$PWD/output/"

# Dtrace script for measuring the overhead of each stage
DTRACE="$SCRIPTDIR/stages.d"

# CSV file which holds the results of the dtrace script
DTRACEFILE="$OUTDIR/stages.csv"

# SLOS setup configuration

# Are we even using striping?
STRIPING=true
# Size of the stripe 
STRIPESIZE="65536"
# Name of the striped disk
STRIPENAME="st0"
# Location of the striped disk
STRIPEDRIVE="/dev/stripe/$STRIPENAME"
# Disks that comprise the stripe
STRIPEDISKS="nvd0 nvd1 nvd2 nvd3"

# Directory on which we mount the 
MOUNTDIR="/testmnt"

# Drive in which the SLOS resides. Equal to the stripe drive only if there
# is one, otherwise $STRIPEDRIVE is invalid
DRIVE="/dev/nvd0"

# Delay in seconds, inserted to avoid certain races between operations
SYNCHDELAY="2"

# The directory from which the server benchmarks are serving
AURWEBROOTDIR="/root/www/"
# TODO: Change these configs to have files of many different sizes

# File of size 0, to be served by the web server benchmarks
AURNULLFILE="nullfile"

# Large file to be served by the web server benchmarks, and associated size
AURBIGFILE="bigfile"
AURBIGFILESIZE="10G"

# Directory that holds the scripts for the SLS
SCRIPTDIR="/$SLSDIR/scripts/"

# Directory that holds the binaries for the SLS benchmarks
SLSBENCHDIR="/root/sls-bench/"

# Location of the firefox script
FIREFOXDIR="$SLSBENCHDIR/firefox/"
FIREFOXSCRIPT="$FIREFOXDIR/run.sh"

# Location of the Javascript benchmarks
JSBENCHMARKPATH="$FIREFOXDIR/hosted"

# Location of the SLSFS directories 
LIGHTLOGDIRSLS="$MOUNTDIR/lighttpd_log"

# Location of the expected lighttpd log directory 
LIGHTLOGDIRUSR="/usr/local/lighttpd"

# Location of the lighttpd binary
LIGHT="/usr/local/sbin/lighttpd"

# The conf file to be used with lighttpd
LIGHTCONFFILE="$SLSDIR/scripts/lighttpd.conf"

# TODO Turn these into variables for wrk
# TODO Add special keys for benchmarking 
LIGHTWRKCONFFILE="$OUTDIR/lighttpd_wrk.conf"

# IP Address of the SLS machine
SLSIP="129.97.75.126"

# Variants of the wrk web benchmark

# Duration of the benchmark
WRKTIME="20"
# Number of threads for the benchmark
WRKTHREAD="5"
# Total number of connections used by the benchmark
WRKCONNS="10"
# Address of the server to be benchmarked
WRKURL="http://"$SLSIP":80/"
# File to be requested by the benchmark
WRKFILE="bigfile"
# Remote to be used as a client
WRKUSER="etsal"
WRKREMOTE="129.97.75.37"
WRKPORT="77"
WRKSSH="$HOME/.ssh/slsbench"


# Name of the memcached binary to be used
MEMCACHEDNAME="memcached"
# Full path of the benchmark
MEMCACHEDPATH="/$SLSBENCHDIR/memcached/$MEMCACHEDNAME"
# Address of the memcached server
MUTILATEADDR="127.0.0.1:11211"
# Path to the mutilate benchmark
MUTILATEPATH="$SLSBENCHDIR/mutilate/mutilate"
MUTILATETIME="60"

# Location of the nginx binary
NGINXBIN="$SLSBENCHDIR/nginx/objs/nginx"

# The directory in which the nginx server is mounted 
NGINXLOGDIRSLS="$MOUNTDIR/nginx_logs"
NGINXLOGDIRUSR="/usr/local/nginx/logs"
NGINXCONFFILE="$SLSDIR/scripts/nginx.conf"

# Location of the binaries.
REDISDIR="/root/sls-bench/redis/"
SERVER="redis-server"
CLIENT="redis-benchmark"
SERVERBIN="$REDISDIR/src/$SERVER"
CLIENTBIN="$REDISDIR/src/$CLIENT"


# Conf 
REDISCONF="$REDISDIR/redis.conf"

# CSV conf 
REDISCSVCONF="$OUTDIR/redis.conf.csv"

# Binary used to dump the Redis config in a nice way
CONFIGDUMPSCRIPT="$SCRIPTDIR/redisconf.py"
# Output used for the Redis results [TODO Make sure it works across ssh]
REDISFILE="$OUTDIR/redis.csv"

# How much time we let the benchmark run
REDISTIME=10

# Directory holding the splash benchmarks
SPLASHDIR="$SLSBENCHDIR/splash/codes/"
# The splash benchmarks we want to run
SPLASHAPPBENCHES="barnes fmm water-nsquared water-spatial"
