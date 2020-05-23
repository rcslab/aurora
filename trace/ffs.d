#pragma D option quiet

BEGIN 
{
    reads = 0;
    writes = 0;
    cmds = 0;
}

fbt::dofilewrite:entry,
fbt::ffs_read:entry,
fbt::ffs_write:entry
{
    current[probefunc] = timestamp;
    this->traceme = 1;
}

fbt::dofilewrite:return,
fbt::ffs_read:return,
fbt::ffs_write:return
{
    @ts[probefunc] = avg(timestamp - current[probefunc]);
    @counts[probefunc] = count();
    @total[probefunc] = sum(timestamp - current[probefunc]);
    this->traceme = 0;
}

END 
{
    printa(@ts);
    printf("Counts\n");
    printa(@counts);
    printf("Sums\n");
    printa(@total);
}
