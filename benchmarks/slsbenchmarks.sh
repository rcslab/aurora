#!/usr/local/bin/bash

mkdir -p results/fs/sls/micro
mkdir results/fs/sls/macro
mkdir results/fs/sls/supermicro
chmod -R a+rw results

./run.py allseries scripts/supermicro 1 200 20 -c skylake.conf -o results/fs/sls/supermicro --type sls --runs 3 --withgstat
./run.py allbenchmarks scripts/micro -c skylake.conf -o results/fs/sls/micro --type sls --runs 4 --withgstat
./run.py allbenchmarks scripts/macro -c skylake.conf -o results/fs/sls/macro --type sls --runs 3 --withgstat
