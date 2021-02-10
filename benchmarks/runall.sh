#!/usr/local/bin/bash

CONF="benchmarks/skylake4.conf"

./benchmarks/run.py webbench --conf "$CONF" --server nginx
./benchmarks/run.py webbench --conf "$CONF" --server lighttpd
./benchmarks/run.py webbench --conf "$CONF" --server tomcat
./benchmarks/run.py kvbench --conf "$CONF" --kvstore redis
./benchmarks/run.py mutilatebench --conf "$CONF"
./benchmarks/run.py ffbench --conf "$CONF"
