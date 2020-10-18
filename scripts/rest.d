#pragma D option quiet
int reststart, resttime;

BEGIN
{
}

fbt::sls_restore:entry
{
    reststart = timestamp;
}

sls:sls::restdone
{
    resttime = timestamp - reststart;
}

END
{
	print("Restore time (ns): %d", resttime);
}
