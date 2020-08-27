#!/usr/local/bin/bash
mkdir -p results/fs/zfs/micro
mkdir -p results/fs/zfs/macro
mkdir -p results/fs/zfs/supermicro
mkdir -p results/fs/zfs/checksum
mkdir -p results/fs/zfs/checksum/micro
mkdir -p results/fs/zfs/checksum/supermicro
mkdir -p results/fs/zfs/checksum/macro
chmod a+rw -R results/fs/zfs

./run.py allbenchmarks scripts/micro -c skylake.conf -o results/fs/zfs/checksum/micro --checksum --runs 3 --type zfs
./run.py allbenchmarks scripts/macro -c skylake.conf -o results/fs/zfs/checksum/macro --checksum --runs 3 --type zfs
./run.py allbenchmarks scripts/supermicro -c skylake.conf -o results/fs/zfs/checksum/supermicro --checksum --runs 3 --type zfs

./run.py allbenchmarks scripts/micro -c skylake.conf -o results/fs/zfs/micro --runs 3 --type zfs
./run.py allbenchmarks scripts/macro -c skylake.conf -o results/fs/zfs/macro --runs 3 --type zfs
./run.py allbenchmarks scripts/supermicro -c skylake.conf -o results/fs/zfs/supermicro --runs 3 --type zfs
