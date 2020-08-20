# NOTE: This file isn't standalone, it needs the definition of $OUTDIR and 
# $SCRIPTDIR. This is by design, since this is supposed to be a helper library.

# Redis benchmark parameters

#Script for the Redis benchmarks. Run at the base Redis directory.
function rdstart {
    # Dump the conf into a file in the output directory
    python3 "$CONFIGDUMPSCRIPT" "$REDISCONF" "$REDISCSVCONF"

    # XXX Dump the benchmark parameters to the output too
    # Run the server in the background
    "$SERVERBIN" "$REDISCONF" &

    # XXX Possible dtrace script here?
}


# Stop the benchmark
function rdstop {
    # Kill the server and stop tracing
    pkill "$SERVER"
    pkill "$CLIENT"
    pkill dtrace

    # Clean up the output
    #rm -rf "$OUTDIR"

    # Clean up any Redis backup files
    rm -f *.rdb *.aof
}

# ------------------------------------------------------------------

# The tests to be run by the client
TESTS="SET,GET"
# Number of redis clients, need a lot for throughput
CLIENTNO="16"
# Number of requests
REQUESTS=$((1024 * 1024 * 32))
# Size of request values controls memory usage along with keyspace
VALSIZE="4096"
# Request pipelining depth, amortizes latency
PIPELINE="10"
#Size of the key space, controls total memory usage
KEYSPACE=$((1024 * 1024 * 1024))

function rdbench {
    # Run the benchmark
    "$CLIENTBIN" -t "$TESTS" -c "$CLIENTNO" -n "$REQUESTS" -d "$VALSIZE" \
	-P "$PIPELINE" -k "$KEYSPACE" &

}
