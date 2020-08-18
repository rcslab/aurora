MEMCACHEDNAME="memcached"
MEMCACHEDPATH="/root/sls-bench/memcached/$MEMCACHEDNAME"
MEMCACHEDADDR="localhost:11211"
MEMCACHEDUSER="root"
MUTILATEPATH="$SLSBENCHDIR/mutilate/mutilate"
MUTILATETIME="60"

function mcstart {
    "$MEMCACHEDPATH" -u "$MEMCACHEDUSER" &
}

function mcbench {
    "$MUTILATEPATH" --server "$MEMCACHEDADDR" -t "$MUTILATETIME"
}

function mcstop {
    pkill "$MEMCACHEDNAME"
}

