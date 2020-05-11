#!/bin/sh

if [ -z $1 ]
then
    echo "Give directory to place runs in as argument"
    exit
fi

gstripe destroy st0
setup_stripe.sh

DRIVE=/dev/stripe/st0
MNT=/testmnt

newfs -j -S 4096 -b 65536 $DRIVE
mount $DRIVE $MNT
cd scripts
for entry in `ls *.f`
do
	echo $entry
	filebench -f $entry > $1/$entry.out
done
cd ..

umount $MNT
gstripe destroy st0
