#!/usr/sbin/dtrace -s

#pragma D option quiet

BEGIN {

}

fbt::sls_write_slos:entry
{
    tstart[probefunc] = timestamp;
}

fbt::sls_write_slos:return
{
    @ts[probefunc] = sum(timestamp - starttime[probefunc])
}

fbt::sls_writemeta_slos:entry
{
    starttime[probefunc] = timestamp;
}

fbt::sls_writemeta_slos:return
{
    @ts[probefunc] = sum(timestamp - starttime[probefunc]);
}

END {
    printa(@ts)
}
