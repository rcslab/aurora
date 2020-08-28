#/usr/local/bin/bash

# TODO find a way to nicely pass the full path of the config file
source "/root/sls/scripts/config.sh"

source "$SCRIPTDIR/aurora.sh"
source "$SCRIPTDIR/redis.sh"
source "$SCRIPTDIR/memcached.sh"
source "$SCRIPTDIR/firefox.sh"
source "$SCRIPTDIR/nginx.sh"
source "$SCRIPTDIR/splash.sh"
source "$SCRIPTDIR/lighttpd.sh"
source "$SCRIPTDIR/wrk.sh"

# ------------------------------------------------------------------

function benchstart {
    mkdir "$AURWEBROOTDIR"
    cd "$AURWEBROOTDIR"

    # Create a bunch of files for the web servers to serve
    touch "$AURNULLFILE"
    truncate -s "$AURBIGFILESIZE" "$AURBIGFILE"

    cd -
}

function redis {
    aurstripe
    aurload

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

    # Since there are multiple nginx processes, get the one with the smallest
    # pid and assume that that is the parent. Add that to the partition.
    ngpid
    slsckpt "$NGINXPID"

    sleep "$SYNCHDELAY"
    ngbench

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
