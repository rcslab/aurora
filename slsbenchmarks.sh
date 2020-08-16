#!/usr/local/bin/bash

mkdir -p results/fs/sls/micro
mkdir results/fs/sls/macro
mkdir results/fs/sls/supermicro
chmod -R a+rw results

#./run.py allseries benchmarks/scripts/supermicro 1 200 20 -c skylake.conf -o results/fs/sls/supermicro --type sls --runs 3 --withgstat
./run.py allbenchmarks benchmarks/scripts/micro -c skylake.conf -o results/fs/sls/micro --type sls --runs 3 --withgstat
./run.py allbenchmarks benchmarks/scripts/macro -c skylake.conf -o results/fs/sls/macro --type sls --runs 3 --withgstat
