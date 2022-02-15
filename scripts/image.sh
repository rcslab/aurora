#!/bin/sh

# Create an Aurora root from a base.txz.

. $SRCROOT/scripts/create_root_subr.sh

create_base_image()
{
    BSDINSTALL_DISTSITE=`get_distribution_mirror`
    BSDINSTALL_DISTDIR="/usr/freebsd-dist"
    BASE=`fetch_base $BSDINSTALL_DISTDIR`

    MNTROOT="/mnt"
    IP=$1
    REPO=$2

    unpack_base $MNTROOT $BASE

    # Mount the pseudo file systems, used during chroot.
    mount_pseudofs $MNTROOT

    configure_dns $MNTROOT $IP
    copy_certs $MNTROOT 
    configure_pkg $MNTROOT $REPO

    umount_pseudofs $MNTROOT

    # Create the image, cleanup, and unmount.
    SLSROOT="$SRCROOT/root.tar"
    pack_base $MNTROOT $SLSROOT
}

if [ -z "$SRCROOT" ]; then
    echo "No SLS source directory specified"
    exit 1
fi

if [ ! -z $SLSROOT ]; then
    echo "No root destination specified"
    exit 0;
fi

if [ -z $IP ]; then
	echo "No DNS server specified, using 8.8.8.8"
	IP="8.8.8.8"
fi

if [ -z $REPO ]; then
	echo "No Aurora package repo specified, using RCS"
	REPO="https://rcs.uwaterloo.ca/~ali/FreeBSD:amd64:12.1/"
fi

create_base_image $IP $REPO
