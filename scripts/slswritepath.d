#!/usr/sbin/dtrace -s

fbt::sls_write_slos:entry
{
    self->writepath = 1;
    starttime[probefunc] = timestamp;
}

fbt::sls_write_slos:return
/self->writepath/
{
    self->writepath = 1;
    @ts[probefunc] = sum(timestamp - starttime[probefunc]);
    self->writepath = 0;
}

fbt::sls_writemeta_slos:entry
/self->writepath/
{
    starttime[probefunc] = timestamp;
}

fbt::sls_writemeta_slos:return
/self->writepath/
{
    @ts[probefunc] = sum(timestamp - starttime[probefunc]);
}

fbt::sls_writedata_slos:entry
/self->writepath/
{
    starttime[probefunc] = timestamp;
}

fbt::sls_writedata_slos:return
/self->writepath/
{
    @ts[probefunc] = sum(timestamp - starttime[probefunc]);
}

fbt::sls_writeobj_data:entry
/self->writepath/
{
    starttime[probefunc] = timestamp;
}

fbt::sls_writeobj_data:return
/self->writepath/
{
    @ts[probefunc] = sum(timestamp - starttime[probefunc]);
}

fbt::slos_iotask_create:entry
/self->writepath/
{
    starttime[probefunc] = timestamp;
}

fbt::slos_iotask_create:return
/self->writepath/
{
    @ts[probefunc] = sum(timestamp - starttime[probefunc]);
}

END
{
    printa(@ts);
}
