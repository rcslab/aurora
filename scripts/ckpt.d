#pragma D option quiet
int ckptstart, ckpttime;

BEGIN
{
}

fbt::sls_checkpoint:entry
{
    ckptstart = timestamp;
}

sls:sls::cont
{
    ckpttime = timestamp - ckptstart;
}

END
{
	print("Checkpoint time (ns): %d", ckpttime);
}
