#!/bin/sh

. $SRCROOT/tests/aurora

DSCRIPT="$SRCROOT/scripts/sloswritepath.d"
REDISSRV="redis-server"
CONF="redis.freebsd.conf"
TMPFILE="redisinput"
SIZE="$1"

if [ -z "$SIZE" ];then
	echo "No size given"
	exit
fi

pkill $REDISSRV

# Wait for the server to die
wait 1

# Create the absolute minimal root for Redis
aursetup
installminroot

# The output of dtrace is piped to the proper file by the parent script.
$DSCRIPT &
DTRACEPID="$!"

cp $CONF "$MNT/$CONF"
chroot $MNT $REDISSRV $CONF &

sleep 1

REDISPID="$( pidof redis-server )"
echo "PID is $REDISPID"

# For some reason, directly piping redisgen.py breaks the pipe.
python3 "$SRCROOT/linuxbench/redis/redisgen.py" "$SIZE"  > "$TMPFILE"
cat "$TMPFILE" | redis-cli --pipe > /dev/null
rm "$TMPFILE"

# Checkpoint to the disk, so that the results are comparable to CRIUs.
slsosdcheckpoint $REDISPID

kill $REDISPID
kill $DTRACEPID

# Wait for the workload to die so that the unmount succeeds.
sleep 1

echo "Wrote $(( $( sysctl -n aurora.data_sent ) + \
    $( sysctl -n aurora.data_received ) )) bytes"

aurteardown
