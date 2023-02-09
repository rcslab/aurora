#!/bin/sh

# Test for snapshot remounting

. aurora

kldload slos
if [ $? -ne 0 ]; then
    echo "Failed to load the modules"
    exit 1
fi

slsnewfs 2> /dev/null
if [ $? -ne 0 ]; then
    echo "Failed to create the SLSFS"
    exit 1
fi

slsmount
if [ $? -ne 0 ]; then
    echo "Failed to mount the SLSFS"
    exit 1
fi

get_last()
{
	slsctl ls -m /testmnt -l
	return $?
}

wait_till_change()
{
    get_last
    CUR_SNAP=$?
    while [ "$(($CUR_SNAP-$1))" = '0' ]
    do
	sleep 1
	get_last
	CUR_SNAP=$?
    done
    SNAP=$CUR_SNAP
}

# Quick test for snapshots, create 4 snapshots, first two creates a file per 
# snapshot, last 2 adds data to those files.
get_last
SNAP=$?
touch $MNT/1.out
wait_till_change $SNAP
touch $MNT/2.out
wait_till_change $SNAP
echo "HELLO" > $MNT/1.out
wait_till_change $SNAP
echo "GOODBYE" > $MNT/2.out
wait_till_change $SNAP

# Only 1.out should be present and no data in 1.out

slsctl ms -m $MNT -i 0

if [ ! -e $MNT/1.out ]; then
    echo "File 1.out is not present in snapshot 0"
    exit 1
fi

if [ -e $MNT/2.out ]; then
    echo "File 2.out is present in snapshot 0"
    exit 1
fi

if [ -s $MNT/1.out ]; then
    echo "File 1.out has data when it should not in snapshot 0"
    exit 1
fi 

# Both files should be present and no data in either
slsctl ms -m $MNT -i 1

if [ ! -e $MNT/1.out ] || [ ! -e $MNT/2.out ]; then
    echo "File 1.out or 2.out is not present in snapshot 1"
    exit 1
fi

if [ -s $MNT/1.out ]; then
    echo "File 1.out has data when it should not in snapshot 1"
    exit 1
fi 

if [ -s $MNT/2.out ]; then
    echo "File 2.out has data when it should not in snapshot 1"
    exit 1
fi 


# Both files should be present and data in file 1
slsctl ms -m $MNT -i 2

if [ ! -e $MNT/1.out ] || [ ! -e $MNT/2.out ]; then
    echo "File 1.out or 2.out is not present in snapshot 2"
    exit 1
fi

if [ ! -s $MNT/1.out ]; then
    echo "File 1.out does not have data when it should in snapshot 2"
    exit 1
fi 

if [ -s $MNT/2.out ]; then
    echo "File 1.out has data when it should not in snapshot 2"
    exit 1
fi 

# Both File should be present and data in both files
slsctl ms -m $MNT -i 3
if [ ! -e $MNT/1.out ] || [ ! -e $MNT/2.out ]; then
    echo "File 1.out or 2.out is not present in snapshot 3"
    exit 1
fi

if [ ! -s $MNT/1.out ] || [ ! -s $MNT/2.out ]; then
    echo "File 1.out does not have data when it should in snapshot 3"
    exit 1
fi 


slsunmount
kldunload slos

exit 0
