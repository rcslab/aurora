#!/bin/sh

# Script that creates a minimal root for benchmarks

# Import the configuration
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
movefile /usr/local/bin/memcached
movefile /usr/local/bin/bash
movefile /lib
movefile /bin
movefile /sbin
movefile /etc
movefile /libexec
movefile /usr/libexec

# Libraries for bash, found using ldd
movefile /usr/local/lib/libreadline.so.8
movefile /usr/local/lib/libhistory.so.8
movefile /lib/libncurses.so.8
movefile /usr/local/lib/libintl.so.8
movefile /usr/lib/libdl.so.1
movefile /lib/libc.so.7
movefile /lib/libncursesw.so.8

# Libraries for Redis, found using ldd
movefile /lib/libthr.so.3
movefile /lib/libm.so.5
movefile /usr/lib/libexecinfo.so.1
movefile /lib/libc.so.7
movefile /lib/libelf.so.2
movefile /lib/libgcc_s.so.1

# Libraries for memcached, found using ldd
movefile /lib/libthr.so.3
movefile /usr/lib/libssl.so.111
movefile /lib/libcrypto.so.111
movefile /usr/local/lib/libevent-2.1.so.7
movefile /usr/local/lib/libsasl2.so.3
movefile /lib/libc.so.7
movefile /usr/lib/libdl.so.1

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
