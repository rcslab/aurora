#!/bin/sh

# Main script, creates the root image

# Import the configuration
SRCROOT="/root/sls"
. "$SRCROOT/tests/aurora"


SCRIPTS="/tmp/slsinstall-script-"
CHROOTSCRIPT="chroot-script.sh"

finddisk

# The install script needs these when partitioning/mounting
export DISK
export MNT
bsdinstall script "$SRCROOT/scripts/bsdinstall.sh"
if [ $? -ne 0 ]; then
    mount
    echo "Setting up BSD root failed with 0"
    exit 1
fi

# Grab the partition created by the script
PARTITIONS="$( ls -1 /dev/ | grep $DISK)"
for PART in $PARTITIONS; do
    PART="/dev/$PART"
    if [ ! -z "$( file -s "$PART" | grep "Unix Fast File system" )" ]; then
	echo "PARTITION IS $PART"
	PARTITION="$PART"
	break
    fi
done

if [ -z $PARTITION ]; then
    echo "No partition found"
    exit 1
fi

# The bsdinstall command nukes /tmp, so splitting the script comes after.
split -a 1 -p '^#!.*' "$SRCROOT/scripts/image.sh" "$SCRIPTS"
chmod u+x "$SCRIPTS"*

# Remount the root its devfs (the devfs is necessary for pkg)
mount -t ufs "$PARTITION" "$MNT"
mount -t devfs devfs "$MNT/dev"
cp "${SCRIPTS}b" "$MNT/$CHROOTSCRIPT"
chroot "$MNT" "/$CHROOTSCRIPT"

# Cleanup and unmount
rm "${SCRIPTS}"*
umount "$MNT/dev"

# Create the image
tar -C "$MNT" -czf "$SLSROOT" .
umount "$MNT"

return

#!/bin/sh

# Post installation script to be run in the chroot

pkg install -y bash;
pkg install -y python;
exit;
