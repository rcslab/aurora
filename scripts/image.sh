#!/bin/sh

# Main script, creates the root image

# Import the configuration
. "$SRCROOT/tests/aurora"

SCRIPTS="/tmp/slsinstall-script-"
CHROOTSCRIPT="chroot-script.sh"


# Parameters related to fetching.
MIRROR="ftp://ftp.freebsd.org"
export BSDINSTALL_DISTDIR="/usr/freebsd-dist"
export DISTRIBUTIONS="base.txz"

_UNAME_R=`uname -r`
case _UNAME_R=${_UNAME_R%-p*} in
*-ALPHA*|*-CURRENT|*-STABLE|*-PRERELEASE)
    RELDIR="snapshots"
    ;;
*)
    RELDIR="releases"
    ;;
esac
DISTSITE="$MIRROR/pub/FreeBSD/${RELDIR}/`uname -m`/`uname -p`/${_UNAME_R}"
export BSDINSTALL_DISTSITE=$DISTSITE

mkdir -p $BSDINSTALL_DISTDIR

# Get the sources if needed. Based on mirrorselect.
if [ ! -f "$BSDINSTALL_DISTDIR/$DISTRIBUTIONS" ]; then
    bsdinstall distfetch

    if [ $? -ne 0 ]; then
	echo "Failed to fetch $DISTRIBUTIONS from $DISTSITE"
	exit 1
    fi
fi

createmd

# The install script needs these when partitioning/mounting.
export DISK
export DISKPATH
export MNT
bsdinstall script "$SRCROOT/scripts/bsdinstall.sh"
if [ $? -ne 0 ]; then
    echo "Setting up BSD root on $MNT failed"
    exit 1
fi

# Grab the partition created by the script.
PARTITIONS="$( ls -1 /dev/ | grep $DISK)"
for PART in $PARTITIONS; do
    PART="/dev/$PART"
    if [ ! -z "$( file -s "$PART" | grep "Unix Fast File system" )" ]; then
	PARTITION="$PART"
	break
    fi
done

if [ -z $PARTITION ]; then
    echo "No partition found"
    echo "Disk is $DISK"
    echo "List of candidate partitions traversed:"
    echo $PARTITIONS
    exit 1
fi

# If the partition was unmounted, remount it.
if [ -z "$( mount | grep $PARTITION)" ]; then
    echo "Remounting $PARTITION on $MNT"
    mount -t ufs "$PARTITION" "$MNT"
    mount -t devfs devfs "$MNT/dev"
fi

# The bsdinstall command nukes /tmp, so splitting the script comes after.
split -a 1 -p '^#!.*' "$SRCROOT/scripts/image.sh" "$SCRIPTS"
chmod u+x "$SCRIPTS"*

cp "${SCRIPTS}b" "$MNT/$CHROOTSCRIPT"
chroot "$MNT" "/$CHROOTSCRIPT"

# Create the image, cleanup, and unmount.
tar -C "$MNT" -czf "$SLSROOT" .

rm "${SCRIPTS}"*
umount "$MNT/dev"
umount "$MNT"
destroymd $MDDISK

return

#!/bin/sh

# Post installation script to be run in the chroot

pkg install -y bash;
pkg install -y python;
pkg install -y redis;

exit;
