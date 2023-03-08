#!/bin/sh
. ../tests/aurora

AURORACTL=$SRCROOT/tools/slsctl/slsctl
BACKEND="slos"
PERIOD="10"
OID="1000"


db_bench() {
    db_bench \
	--benchmarks=fillbatch,mixgraph \
	--use_direct_io_for_flush_and_compaction=true \
	--use_direct_reads=true \
	--cache_size=$((256 << 20)) \
	--key_dist_a=0.002312 \
	--key_dist_b=0.3467 \
	--keyrange_dist_a=14.18 \
	--keyrange_dist_b=0.3467 \
	--keyrange_dist_c=0.0164 \
	--keyrange_dist_d=-0.08082 \
	--keyrange_num=30 \
	--value_k=0.2615 \
	--value_sigma=25.45 \
	--iter_k=2.517 \
	--iter_sigma=14.236 \
	--mix_get_ratio=0.83 \
	--mix_put_ratio=0.14 \
	--mix_seek_ratio=0.03 \
	--sine_mix_rate_interval_milliseconds=5000 \
	--sine_a=1000 \
	--sine_b=0.000073 \
	--sine_d=4500 \
	--perf_level=2 \
	--num=$ROCKSDB_NUM \
	--key_size=48 \
	--db=/testmnt/tmp-db \
	--wal_dir=/testmnt/wal \
	--duration=$ROCKSDB_DUR \
	--histogram=1 \
	--write_buffer_size=$((16 << 30)) \
	--disable_auto_compactions \
	--threads=24 \
	"${@:2}"
    cd -
    return $?
}

stress_aurora()
{

	aursetup
	$AURORACTL partadd $BACKEND -o $OID -d -t $PERIOD 

	db_bench baseline --sync=false --disable_wal=true &
	FUNC_PID="$!"
	sleep 1

	pid=`pidof db_bench`
	$AURORACTL attach -o $OID -p $pid 2>> $LOG >> $LOG
	$AURORACTL checkpoint -o $OID -r >> $LOG 2>> $LOG

	wait $FUNC_PID
	if [ $? -eq 124 ];then
		echo "[Aurora] Issue with db_bench, restart required"
		exit 1
	fi
	sleep 2

	aurteardown
}

for i in `seq 1 5` ; do 
	stress_aurora
done


echo "[DONE] Done...\n"

