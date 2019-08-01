#pragma D option quiet
BEGIN 
{
}

fbt:sls::entry, 
fbt::vmspace_fork:entry, 
fbt::swap_reserve_by_cred:entry,
fbt::vmspace_free:entry
{
	current[probefunc] = timestamp;
}

fbt:sls::return, 
fbt::vmspace_fork:return, 
fbt::swap_reserve_by_cred:return,
fbt::vmspace_free:return
{
	@ts[probefunc] = avg(timestamp - current[probefunc]);
}

END 
{
	TO_MICRO = 1000;
	normalize(@ts, TO_MICRO);
	printa(@ts);
}
