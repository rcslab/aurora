#!/usr/sbin/dtrace -s

#pragma D option quiet
unsigned long checkpoint_start, last_checkpoint_event;
unsigned long stopclock_start, meta_start;

BEGIN {
    checkpoint_start = timestamp;
    last_checkpoint_event = timestamp;
    stopclock_start = timestamp;
    meta_start = timestamp;
}

fbt::sls_ckpt:entry
{
    checkpoint_start = timestamp;
    last_checkpoint_event = timestamp;
    stopclock_start = timestamp;
    meta_start = timestamp;
}

sls:::stopclock_start
{
	stopclock_start = timestamp;
}


sls:::meta_start
{
	meta_start = timestamp;
}

sls:::meta_finish
{
    	@tavg["Metadata copy"] = avg(timestamp - meta_start);
}

fbt::slsckpt_filedesc:entry
{
	tstart["filedesc"] = timestamp;
}

fbt::slsckpt_filedesc:return
{
    @tavg["filedesc"] = avg(timestamp - tstart["filedesc"]);
}

fbt::slsckpt_file:entry
{
	tstart["file"] = timestamp;
}

fbt::slsckpt_file:return
{
    @tavg["file"] = avg(timestamp - tstart["file"]);
}

sls:::stopclock_start
{
    stopclock_start = timestamp;
}

sls:::stopclock_finish
{
    @tavg["Application stop time"] = avg(timestamp - checkpoint_start);
    proc_last_checkpoint_event = timestamp;
}

sls::sls_ckpt:
{
    @tavg[stringof(arg0)] = avg(timestamp - last_checkpoint_event);
    last_checkpoint_event = timestamp;
}

fbt::sls_ckpt:return
{
    @tavg["Total time"] = avg(timestamp - checkpoint_start);
}

fbt::slos_iotask_create:entry
{
    task_start = timestamp;
}


fbt::slos_iotask_create:return
{
    task_start = timestamp;
    @tavg["Task IO"] = avg(timestamp - checkpoint_start);
}

END
{
    printa(@tavg);
}
