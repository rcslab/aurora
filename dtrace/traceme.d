uint64_t checkpoint;

fbt::slos_rwrite:entry
{
    self->traceme = 1;
    times[probefunc] = timestamp;
}

fbt:::entry
/self->traceme/
{
    times[probefunc] = timestamp;
}

fbt:::return
/self->traceme/
{
    @ts[probefunc] = avg(timestamp - times[probefunc]);
    @total[probefunc] = sum(timestamp - times[probefunc]);
}




fbt::slos_rwrite:return
/self->traceme/
{
    @ts[probefunc] = avg(timestamp - times[probefunc]);
    self->traceme = 0;
    trace(pid);
}

END
{
    printa(@total);
    printa(@ts);
}
