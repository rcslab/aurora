#!/bin/sh

REDISSRV="redis-server"
CKPTDIR="$PWD/criuimage"
CONF="redis.linux.conf"
TMPFILE="redisinput"
SIZE="$1"

if [ -z "$SIZE" ];then
	echo "No size given"
	exit
fi

pkill $REDISSRV

# Wait for the server to die.
sleep 1

setsid $REDISSRV $CONF &

# Wait for the server to spin up.
sleep 1

# We have to get it by PID, we put the server in
# a new session and $! points to setsid's PID.
REDISPID=$(pidof $REDISSRV)

# For some reason, directly piping redisgen.py breaks the pipe.
"$SRCROOT/linuxbench/redis/redisgen.py" "$SIZE"  > "$TMPFILE"
cat "$TMPFILE" | redis-cli --pipe > /dev/null
rm "$TMPFILE"

mkdir -p $CKPTDIR
sudo mount -t tmpfs -o size=10g tmpfs $CKPTDIR
criu dump -D $CKPTDIR --shell-job -t $REDISPID --display-stats 2> /dev/null
sudo umount $CKPTDIR
rm -r $CKPTDIR
