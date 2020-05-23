#!/bin/sh

if [ -z $1 ]
then
    echo "Give directory to place runs in as argument"
    exit
fi

MNT=/testmnt

for run in $(seq 1 $2)
do
	echo ""
	echo "Run $run of $2 started..."
	echo ""

	zpool create benchmark /dev/nvd0 /dev/nvd1
	zfs create benchmark/testmnt

	zfs set mountpoint=/testmnt benchmark/testmnt
	zfs set compression=lz4 benchmark
	zfs set recordsize=64k benchmark
	# Uncomment line below to disable the ZIL
	zfs set sync=disabled benchmark
	#zfs set sync=default benchmark

	DIR=$1/$run
	mkdir $DIR

	cd scripts
	for entry in `ls *.f`
	do
		echo $entry
		filebench -f $entry > $DIR/$entry.out
	done
	cd ..

	zfs destroy -r benchmark/testmnt
	zpool destroy benchmark
done;

