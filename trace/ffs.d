#pragma D option quiet

BEGIN 
{
    reads = 0;
    writes = 0;
    cmds = 0;
}

fbt::ffs_read:entry,
fbt::ffs_write:entry
{
    current[probefunc] = timestamp;
    this->traceme = 1;
}

/*
io:::start
/this->traceme && args[0] != NULL/
{
    @disk[args[1]->device_name, args[1]->unit_number, args[0]->bio_cmd] = count();
}
*/
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
/*    printa(@disk); */
}
