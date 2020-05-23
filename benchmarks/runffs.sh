#!/bin/sh

if [ -z $1 ]
then
    echo "Give directory to place runs in as argument"
    exit
fi

if [ -z $2 ]
then
    echo "Require number of runs"
    exit
fi


#gstripe destroy st0
gstripe create -s 65536 -v st0 nvd0 nvd1 #nvd2 nvd3

DRIVE=/dev/stripe/st0

MNT=/testmnt

for run in $(seq 1 $2)
do
	DIR=$1/$run
	mkdir $DIR

	echo ""
	echo "Run $run of $2 started..."
	echo ""

	newfs -j -S 4096 -b 65536 $DRIVE
	mount $DRIVE $MNT
	cd scripts
	for entry in `ls *.f`
	do
		echo $entry
		filebench -f $entry > $DIR/$entry.out
	done
	cd ..
	umount $MNT
done;
gstripe destroy st0


