#!/bin/sh

# Script that creates a minimal root for benchmarks

# Import the configuration
SRCROOT="/root/sls"
. "$SRCROOT/tests/aurora"

TMPSRC="/mnt/src"
TMPDST="/mnt/dst"

movefile()
{
    FILE=$1
    if [ -z $FILE ]; then
	return
    fi

    DIR="$(dirname $FILE)"
    if [ ! -z "$DIR" ]; then
	mkdir -p "$TMPDST/$DIR"
    fi
    cp -r "$TMPSRC/$FILE" "$TMPDST/$FILE"
    echo "Moving $FILE"
}

crdir()
{
    DIR=$1
    if [ -z $DIR ]; then
	return
    fi

    mkdir -p "$TMPDST/$DIR"

}

if [ -f "$SLSMINROOT" ]; then
    return
fi

mkdir -p "$TMPSRC"
mkdir -p "$TMPDST"

tar -C "$TMPSRC" -xf "$SLSROOT"

# Elf interpreter
movefile /libexec/ld-elf.so.1
movefile /usr/local/bin/redis-server
# Libraries for Redis, found using ldd
movefile /lib/libthr.so.3
movefile /lib/libm.so.5
movefile /usr/lib/libexecinfo.so.1
movefile /lib/libc.so.7
movefile /lib/libelf.so.2
movefile /lib/libgcc_s.so.1

crdir etc
crdir data
crdir var
crdir var/cache
crdir var/run
crdir log
crdir logs
crdir dev

tar -C "$TMPDST" -czf "$SLSMINROOT" .

# Change permissions
chflags -R noschg,nosunlink $TMPSRC
chflags -R noschg,nosunlink $TMPDST
rm -rf $TMPSRC
rm -rf $TMPDST
