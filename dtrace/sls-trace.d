#pragma D option quiet

BEGIN 
{
}

fbt:sls::entry, 
fbt:slos::entry,
fbt::vmspace_fork:entry, 
fbt::swap_reserve_by_cred:entry,
fbt::vmspace_free:entry
{
	current[probefunc] = timestamp;
}

sls:::stopped
{
	current["stopped"] = timestamp;
}

sls:::cont
{
	@ts["SIGSTOP to SIGCONT"] = avg(timestamp - current["stopped"]);
	@counts["SIGSTOP to SIGCONT"] = count();
}

fbt:sls::return, 
fbt:slos::return,
fbt::vmspace_fork:return, 
fbt::swap_reserve_by_cred:return,
fbt::vmspace_free:return
{
	@ts[probefunc] = avg(timestamp - current[probefunc]);
	@total[probefunc] = sum(timestamp - current[probefunc]);
	@counts[probefunc] = count();
}

END 
{
	TO_MICRO = 1000;
	normalize(@ts, TO_MICRO);
	printa(@ts);
	printf("Counts\n");
	printa(@counts);
}
