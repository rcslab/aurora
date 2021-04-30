#!/usr/sbin/dtrace -s

#pragma D option quiet
unsigned long checkpoint_start, last_checkpoint_event;
unsigned long stopclock_start;

BEGIN {
    checkpoint_start = timestamp;
    last_checkpoint_event = timestamp;
    stopclock_start = timestamp;
}

fbt::sls_ckpt:entry
{
    checkpoint_start = timestamp;
    last_checkpoint_event = timestamp;
    stopclock_start = timestamp;
}

fbt::slsckpt_filedesc:entry
{
	tstart["filedesc"] = timestamp;
}

fbt::slsckpt_filedesc:return
{
    @tquantize["filedesc"] = quantize(timestamp - tstart["filedesc"]);
    @tavg["filedesc"] = avg(timestamp - tstart["filedesc"]);
    @tsum["filedesc"] = sum(timestamp - tstart["filedesc"]);
}

fbt::slsckpt_file:entry
{
	tstart["file"] = timestamp;
}

fbt::slsckpt_file:return
{
    @tquantize["file"] = quantize(timestamp - tstart["file"]);
    @tavg["file"] = avg(timestamp - tstart["file"]);
    @tsum["file"] = sum(timestamp - tstart["file"]);
}

sls:::stopclock_start
{
    stopclock_start = timestamp;
}

sls:::stopclock_finish
{
    @tquantize["Application stop time"] = quantize(timestamp - checkpoint_start);
    @tavg["Application stop time"] = avg(timestamp - checkpoint_start);
    @tsum["Application stop time"] = sum(timestamp - checkpoint_start);
    proc_last_checkpoint_event = timestamp;
}

sls::sls_ckpt:
{
    @tquantize[stringof(arg0)] = quantize(timestamp - last_checkpoint_event);
    @tavg[stringof(arg0)] = avg(timestamp - last_checkpoint_event);
    @tsum[stringof(arg0)] = sum(timestamp - stopclock_start);
    last_checkpoint_event = timestamp;
}

fbt::sls_ckpt:return
{
    @tquantize["Total time"] = quantize(timestamp - checkpoint_start);
    @tavg["Total time"] = avg(timestamp - checkpoint_start);
}

fbt::slos_iotask_create:entry
{
    task_start = timestamp;
}


fbt::slos_iotask_create:return
{
    task_start = timestamp;
    @tquantize["Task IO"] = quantize(timestamp - checkpoint_start);
    @tavg["Task IO"] = avg(timestamp - checkpoint_start);
    @tsum["Task IO"] = sum(timestamp - checkpoint_start);
}

END
{
    printa(@tquantize);
    printa(@tavg);
    printa(@tsum);
}
