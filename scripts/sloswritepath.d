#!/usr/sbin/dtrace -s

fbt::slos_iotask_create:entry
{
    self->writepath = 1;
    starttime[probefunc] = timestamp;
}

fbt::slos_iotask_create:return
/self->writepath/
{
    self->writepath = 1;
    @ts[probefunc] = sum(timestamp - starttime[probefunc]);
    @tavg[probefunc] = avg(timestamp - starttime[probefunc]);
    self->writepath = 0;
}

fbt::slos_io:entry
/self->writepath/
{
    starttime[probefunc] = timestamp;
}

fbt::slos_io:return
/self->writepath/
{
    @ts[probefunc] = sum(timestamp - starttime[probefunc]);
    @tavg[probefunc] = avg(timestamp - starttime[probefunc]);
}


fbt::slos_io_setdaddr:entry
/self->writepath/
{
    starttime[probefunc] = timestamp;
}

fbt::slos_io_setdaddr:return
/self->writepath/
{
    @ts[probefunc] = sum(timestamp - starttime[probefunc]);
    @tavg[probefunc] = avg(timestamp - starttime[probefunc]);
}

fbt::slos_io_physaddr:entry
/self->writepath/
{
    starttime[probefunc] = timestamp;
}

fbt::slos_io_physaddr:return
/self->writepath/
{
    @ts[probefunc] = sum(timestamp - starttime[probefunc]);
    @tavg[probefunc] = avg(timestamp - starttime[probefunc]);
}

fbt::slos_io_physaddr:entry
/self->writepath/
{
    starttime[probefunc] = timestamp;
}

fbt::slos_io_physaddr:return
/self->writepath/
{
    @ts[probefunc] = sum(timestamp - starttime[probefunc]);
    @tavg[probefunc] = avg(timestamp - starttime[probefunc]);
}

fbt::taskqueue_enqueue:entry
/self->writepath/
{
    starttime[probefunc] = timestamp;
}

fbt::taskqueue_enqueue:return
/self->writepath/
{
    @ts[probefunc] = sum(timestamp - starttime[probefunc]);
    @tavg[probefunc] = avg(timestamp - starttime[probefunc]);
}

END
{
    printa(@ts);
    printa(@tavg);
}
