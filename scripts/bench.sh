#/usr/local/bin/bash

SCRIPTDIR="/root/sls/scripts/"

source "$SCRIPTDIR/aurora.sh"
source "$SCRIPTDIR/redis.sh"
source "$SCRIPTDIR/memcached.sh"
source "$SCRIPTDIR/firefox.sh"
source "$SCRIPTDIR/nginx.sh"
source "$SCRIPTDIR/splash.sh"
source "$SCRIPTDIR/lighttpd.sh"

SYNCHDELAY="4"

# The directory from which the server benchmarks are serving
AURWEBROOTDIR="/root/www/"
AURNULLFILE="nullfile"
AURBIGFILE="bigfile"
AURBIGFILESIZE="10G"

# ------------------------------------------------------------------

function benchstart {
    mkdir "$AURWEBROOTDIR"
    cd "$AURWEBROOTDIR"

    touch "$AURNULLFILE"
    truncate -s "$AURBIGFILESIZE" "$AURBIGFILE"

    cd -
}

REDISTIME=10

function redis {
    aurstripe
    aurload

    # Dump the configuration settings to the output file
    echo "REDISTIME,$REDISTIME" >> "$REDISBENCHFILE"

    rdstart
    sleep "$SYNCHDELAY"

    slsckpt `pidof redis-server`

    rdbench

    sleep $REDISTIME

    rdstop
    aurunload
}

function memcached {
    aurstripe
    aurload
    mcstart

    slsckpt `pidof "$MEMCACHEDNAME"`
    sleep "$SYNCHDELAY"

    mcbench

    mcstop
    aurunload
}

function firefox() {
    aurstripe
    aurload
    ffstart

    nohup ffbench &
    sleep "$SYNCHDELAY"

    slsckpt `pidof geckodriver`

    wait

    ffstop
    aurunload
}

function nginx() {
    aurstripe
    aurload
    ngstart

    ngbench

    # Since there are multiple nginx processes, get the one with the smallest
    # pid and assume that that is the parent. Add that to the partition.
    ngpid
    slsckpt "$NGINXPID"

    ngstop
    aurunload
}

function splash() {
    aurstripe
    aurload

    spbench &
    SPLASHPID=$!

    slsckpt "$SPLASHPID"

    wait

    aurunload
}

function light() {
    aurstripe
    aurload
    listart

    sleep "$SYNCHDELAY"
    slsckpt `pidof lighttpd`
    libench

    listop
    aurunload
}

function benchstop {
    rm -r "$OUTFILE"
    rm -rf "$AURWEBROOTDIR"
    aurstripe
}
