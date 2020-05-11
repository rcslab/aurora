#!/bin/sh

if [ -z $1 ]
then
    echo "Give directory to place runs in as argument"
    exit
fi

MNT=/testmnt
zpool create benchmark /dev/nvd0 /dev/nvd1 /dev/nvd2 /dev/nvd3

zfs create benchmark/testmnt

zfs set mountpoint=/testmnt benchmark/testmnt
# Uncomment line below to disable the ZIL
#zfs set sync=disabled

cd scripts
for entry in `ls *.f`
do
	echo $entry
	filebench -f $entry > $1/$entry.out
done
cd ..

zfs destroy -r benchmark/testmnt
zpool destroy benchmark

