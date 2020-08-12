#!/usr/local/bin/bash
SYNC=0
SECONDS=0
while ((SECONDS < $1)); do
	sync /testmnt
	((SYNC++))
done


echo $SYNC

