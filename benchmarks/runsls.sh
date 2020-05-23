#!/bin/sh

if [ -z $1 ]
then
    echo "Give directory to place runs in as argument"
    exit
fi

DRIVE=/dev/stripe/st0
MNT=/testmnt

#gstripe create -s 65536 -v st0 nvd0 nvd1 #nvd2 nvd3

echo ""
echo "Run started..."
echo ""

cd scripts
for entry in `ls *.f`
do
	#../../tools/newosd/newosd $DRIVE
	#kldload ../../slos/slos.ko
	#mount -rw -t slsfs $DRIVE /testmnt

	#echo $entry
	filebench -f $entry > $1/$entry.out

	#umount $MNT
	#kldunload slos.ko
done
cd ..


