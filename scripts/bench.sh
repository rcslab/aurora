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

REDISTIME=10


# ------------------------------------------------------------------

function redis {
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
    aurload
    mcstart

    slsckpt `pidof "$MEMCACHEDNAME"`
    sleep "$SYNCHDELAY"

    mcbench

    mcstop
    aurunload
}

function firefox() {
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
    aurload

    spbench &
    SPLASHPID=$!

    slsckpt "$SPLASHPID"

    wait

    aurunload
}

function light() {
    aurload
    listart

    sleep "$SYNCHDELAY"
    slsckpt `pidof lighttpd`
    libench

    listop
    aurunload
}

function slsoutclean {
    rm -r "$OUTFILE"
}
