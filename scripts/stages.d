#pragma D option quiet
BEGIN 
{
	start = 0;
	stopped = 0;
	sysv = 0;
	proc = 0;
	vm = 0;
	file = 0;
	shadow = 0;
	cont = 0;
	dump = 0;
	done = 0;

	stoptime = 0;
	sysvtime = 0;
	proctime = 0;
	vmtime = 0;
	filetime = 0;
	shadowtime = 0;
	conttime = 0;
	dumptime = 0;
	deduptime = 0;
	synctime = 0;

	iters = 0;
}

sls:sls::start
{
	start = timestamp;
}

sls:sls::stopped
{
	stopped = timestamp;
	stoptime += stopped - start;
	iters += 1;
}

sls:sls::sysv
{
	sysv = timestamp;
	sysvtime += sysv - stopped;
}

sls:sls::proc
{
	proc = timestamp;
	proctime += proc - sysv;
}

sls:sls::vm
{
	vm = timestamp;
	vmtime += vm - proc;
}

sls:sls::file
{
	file = timestamp;
	filetime += file - vm;
}

sls:sls::shadow
{
	shadow = timestamp;
	shadowtime += shadow - file;
}

sls:sls::cont
{
	cont = timestamp;
	conttime += cont - shadow;
}

sls:sls::dump
{
	dump = timestamp;
	dumptime += dump - cont;
}

sls:sls::sync
{
	sync = timestamp;
	synctime += sync - dump;
}

sls:sls::dedup
{
	dedup = timestamp;
	deduptime += dedup - sync;
}

END
{
	printf("Iterations: %d\n", iters);
	printf("%s, %u\n", "stop", stoptime / iters);
	printf("%s, %u\n", "sysv", sysvtime / iters);
	printf("%s, %u\n", "proc", proctime / iters);
	printf("%s, %u\n", "vm", vmtime / iters);
	printf("%s, %u\n", "file", filetime / iters);
	printf("%s, %u\n", "shadow", shadowtime / iters);
	printf("%s, %u\n", "cont", conttime / iters);
	printf("%s, %u\n", "dump", dumptime / iters);
	printf("%s, %u\n", "sync", synctime / iters);
	printf("%s, %u\n", "dedup", deduptime / iters);
}
