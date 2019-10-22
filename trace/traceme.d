uint64_t checkpoint;

fbt::slsfs_mount:entry
{
    self->traceme = 1;
    times[probefunc] = timestamp;
}

fbt:::entry
/self->traceme/
{
    printf("%s CALL", probefunc);
    times[probefunc] = timestamp;
}

fbt:::return
/self->traceme/
{
    printf("%s RETURN", probefunc);
    /*@ts[probefunc] = avg(timestamp - times[probefunc]);*/
    /*@total[probefunc] = sum(timestamp - times[probefunc]);*/
}

fbt::slsfs_mount:return
/self->traceme/
{
    /*@ts[probefunc] = avg(timestamp - times[probefunc]);*/
    self->traceme = 0;
    trace(pid);
}

