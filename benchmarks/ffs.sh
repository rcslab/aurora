#!/usr/local/bin/bash
mkdir -p results/fs/ffs/micro
mkdir -p results/fs/ffs/macro
mkdir -p results/fs/ffs/supermicro
chmod a+rw -R results/fs/ffs

./run.py allbenchmarks scripts/micro -c skylake.conf -o results/fs/ffs/micro --runs 3 --withgstat --type ffs
./run.py allbenchmarks scripts/macro -c skylake.conf -o results/fs/ffs/macro --runs 3 --withgstat --type ffs
./run.py allbenchmarks scripts/supermicro -c skylake.conf -o results/fs/ffs/supermicro --runs 3 --withgstat --type ffs
