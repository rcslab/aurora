function mcstart {
    "$MEMCACHEDPATH" -u $USER &
}

function mcbench {
    "$MUTILATEPATH" --server "$MUTILATEADDR" -t "$MUTILATETIME"
}

function mcstop {
    pkill "$MEMCACHEDNAME"
}

