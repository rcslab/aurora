#!/bin/sh

FILESIZE=$(( 1024 * 1024 ))
NUMFILES=1024

# Test whether 
make_file_with_data(){
	TMPFILE=`mktemp /tmp/slstemp.XXXXX`
	dd if=/dev/random of=/$TMPFILE bs=$FILESIZE count=1
	echo $TMPFILE
}

. aurora

# Load the SLOS and mount the SLSFS
loadslos
slsnewfs
slsmount

# Create 1K files outside of the SLOS, copy them into it
# Create the one file we will check in the SLOS.
INITIAL=`make_file_with_data`
INITIAL_INSLSFS="$MNT/initial"
cp $INITIAL $MNT/initial

# Create a file and load it up into the SLOS using 
# different names.
TMPFILE=`make_file_with_data`
for i in `seq 1 $(( $NUMFILES - 1 ))`; do
	SLSTMP=`mktemp $MNT/slstemp.XXXXX`
	cp $TMPFILE $SLSTMP
done
rm $TMPFILE

# Read back the initial file and test for differences.
INITIAL_READBACK="/tmp/readback"
cp $INITIAL_INSLSFS $INITIAL_READBACK

if [ ! -z `diff $INITIAL $INITIAL_READBACK` ]; then
	rm $INITIAL_READBACK
	rm $INITIAL

	echo "File read back differs from initial"
	exit 1
fi

rm $INITIAL_READBACK
rm $INITIAL

slsunmount
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

unloadslos
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

exit 0
