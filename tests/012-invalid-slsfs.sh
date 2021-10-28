#!/bin/sh

. aurora

# Destroy any existing ramdisks that may have valid FSes from previous tests.
destroymd $MDDISK
createmd

loadslos
if [ $? -ne 0 ]; then
	echo "Failed to load SLOS"
	exit 1
fi

# We attempt to mount the invalid FS multiple times to ensure we abort the mount
# cleanly in the kernel. Improper error handling in the kernel (e.g. forgetting to 
# release taken locks before aborting the mount) can freeze the system on multiple
# mount attempts.
for i in `seq 5`
do
	slsmount 2>&1
	if [ $? -eq 0 ]; then
		echo "Loaded corrupted SLOS"
		exit 1
	fi

	sleep 1
done

unloadslos
if [ $? -ne 0 ]; then
	echo "Failed to unload SLOS"
	exit 1
fi


