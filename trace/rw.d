#pragma D option quiet

BEGIN 
{
    reads = 0;
    writes = 0;
    cmds = 0;
}

fbt::slsfs_read:entry,
fbt::slsfs_write:entry,
fbt::slsfs_retrieve_buf:entry,
fbt::slsfs_bcreate:entry,
fbt::slsfs_bread:entry,
fbt::dofilewrite:entry,
fbt::slsfs_strategy:entry
{
    current[probefunc, tid] = timestamp;
    this->traceme = 1;
}

fbt::slsfs_read:return,
fbt::slsfs_write:return,
fbt::slsfs_retrieve_buf:return,
fbt::slsfs_bcreate:return,
fbt::slsfs_bread:return,
fbt::dofilewrite:return,
fbt::slsfs_strategy:return
{
    @ts[probefunc, tid] = avg(timestamp - current[probefunc, tid]);
    @counts[probefunc, tid] = count();
    @total[probefunc, tid]= sum(timestamp - current[probefunc, tid]);
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
