#!/usr/local/bin/bash

set -e

SELF="${BASH_SOURCE[0]}"
SLSDIR="$(realpath "$(dirname "$SELF")/..")"
ROCKSDB="$SLSDIR/../rocksdb"
YCSB="$SLSDIR/../YCSB"

WORKDIR="$1/tmp"
mkdir -p "$WORKDIR"

SPAWN="$SLSDIR/tools/slsctl/slsctl spawn -o 1000 --"

build_rocksdb() {
    JAVA_HOME=/usr/local/openjdk8 cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DFAIL_ON_WARNINGS=OFF \
        -DWITH_SNAPPY=ON \
        -DWITH_JNI=ON \
        -DCUSTOM_REPO_URL="https://repo1.maven.org/maven2" \
        -DSLS_PATH="$SLSDIR"
    make -j$(gnproc) rocksdbjni-shared
    ln -s librocksdbjni-shared.so java/librocksdbjni.so
}

if [ ! -e "$ROCKSDB/build-baseline/java/librocksdbjni.so" ]; then
    (
        cd "$ROCKSDB"
        git checkout sls-baseline
        rm -rf build-baseline
        mkdir build-baseline
        cd build-baseline
        build_rocksdb
    )
fi

if [ ! -e "$ROCKSDB/build-sls/java/librocksdbjni.so" ]; then
    (
        cd "$ROCKSDB"
        git checkout sls
        rm -rf build-sls
        mkdir build-sls
        cd build-sls
        build_rocksdb
    )
fi

bench_rocksdb() {
    (
        # https://github.com/brianfrankcooper/YCSB/wiki/Core-Workloads

        cd "$YCSB"
        rm -rf "$WORKDIR/ycsb-rocksdb-data"

        echo "======== Load workload A ========" >../rocksdb-ycsb-$1.log
        $2 python2 ./bin/ycsb load rocksdb -s -P workloads/workloada -p rocksdb.dir="$WORKDIR/ycsb-rocksdb-data" -jvm-args "-Djava.library.path=$ROCKSDB/build-$1/java " >>../rocksdb-ycsb-$1.log || true

        echo "======== Run workload A ========" >>../rocksdb-ycsb-$1.log
        $2 python2 ./bin/ycsb run rocksdb -s -P workloads/workloada -p rocksdb.dir="$WORKDIR/ycsb-rocksdb-data" -jvm-args "-Djava.library.path=$ROCKSDB/build-$1/java " >>../rocksdb-ycsb-$1.log || true
        echo "======== Run workload B ========" >>../rocksdb-ycsb-$1.log
        $2 python2 ./bin/ycsb run rocksdb -s -P workloads/workloadb -p rocksdb.dir="$WORKDIR/ycsb-rocksdb-data" -jvm-args "-Djava.library.path=$ROCKSDB/build-$1/java " >>../rocksdb-ycsb-$1.log || true
        echo "======== Run workload C ========" >>../rocksdb-ycsb-$1.log
        $2 python2 ./bin/ycsb run rocksdb -s -P workloads/workloadc -p rocksdb.dir="$WORKDIR/ycsb-rocksdb-data" -jvm-args "-Djava.library.path=$ROCKSDB/build-$1/java " >>../rocksdb-ycsb-$1.log || true
        echo "======== Run workload F ========" >>../rocksdb-ycsb-$1.log
        $2 python2 ./bin/ycsb run rocksdb -s -P workloads/workloadf -p rocksdb.dir="$WORKDIR/ycsb-rocksdb-data" -jvm-args "-Djava.library.path=$ROCKSDB/build-$1/java " >>../rocksdb-ycsb-$1.log || true
        echo "======== Run workload D ========" >>../rocksdb-ycsb-$1.log
        $2 python2 ./bin/ycsb run rocksdb -s -P workloads/workloadd -p rocksdb.dir="$WORKDIR/ycsb-rocksdb-data" -jvm-args "-Djava.library.path=$ROCKSDB/build-$1/java " >>../rocksdb-ycsb-$1.log || true

        rm -rf "$WORKDIR/ycsb-rocksdb-data"

        echo "======== Load workload E ========" >>../rocksdb-ycsb-$1.log
        $2 python2 ./bin/ycsb load rocksdb -s -P workloads/workloade -p rocksdb.dir="$WORKDIR/ycsb-rocksdb-data" -jvm-args "-Djava.library.path=$ROCKSDB/build-$1/java " >>../rocksdb-ycsb-$1.log || true
        echo "======== Run workload E ========" >>../rocksdb-ycsb-$1.log
        $2 python2 ./bin/ycsb run rocksdb -s -P workloads/workloade -p rocksdb.dir="$WORKDIR/ycsb-rocksdb-data" -jvm-args "-Djava.library.path=$ROCKSDB/build-$1/java " >>../rocksdb-ycsb-$1.log || true
    )
}

bench_rocksdb baseline ""
bench_rocksdb sls "$SPAWN"
