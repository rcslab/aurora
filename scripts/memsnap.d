#!/usr/sbin/dtrace -s

#pragma D option quiet
int memsnap_start, last_memsnap_event;
int stopclock_start, stopclock_finish;

fbt::slsckpt_dataregion:entry
{
    memsnap_start = last_memsnap_event = timestamp;
}

sls::slsckpt_dataregion:
{
    @tround[stringof(arg0)] = sum(timestamp - last_memsnap_event);
    @tavg[stringof(arg0)] = avg(timestamp - last_memsnap_event);
    @ttotal[stringof(arg0)] = sum(timestamp - last_memsnap_event);

    last_memsnap_event = timestamp;
}

sls:::stopclock_start
{
    stopclock_start = timestamp;
}

sls:::stopclock_finish
{
    stopclock_finish = timestamp;
    @tround["Stop time"] = sum(stopclock_finish - stopclock_start);
    @tavg["Stop time"] = avg(stopclock_finish - stopclock_start);
    @ttotal["Stop time"] = sum(stopclock_finish - stopclock_start);
}

sls::slsckpt_dataregion_fillckpt:
{
    @tround[stringof(arg0)] = sum(timestamp - last_memsnap_event);
    @tavg[stringof(arg0)] = avg(timestamp - last_memsnap_event);
    @ttotal[stringof(arg0)] = sum(timestamp - last_memsnap_event);
    last_memsnap_event = timestamp;
}

fbt::slsckpt_dataregion:return
{
    @tround["Total time"] = sum(last_memsnap_event - memsnap_start);
    @tavg["Total time"] = avg(last_memsnap_event - memsnap_start);
    @ttotal["Total time"] = sum(last_memsnap_event - memsnap_start);
    printf("\nIteration:\n");
    printa(@tround);
    clear(@tround);
}

END
{
    printf("\nAverages:\n");
    printa(@tavg);
    printf("\nTotals:\n");
    printa(@ttotal);

}
