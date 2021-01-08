#pragma D option quiet
int checkpoint_start, last_checkpoint_event;
int proc_checkpoint_start, proc_last_checkpoint_event;
int stopclock_start, stopclock_finish;

BEGIN
{
}

fbt::slsckpt_metadata:entry
{
    proc_checkpoint_start = proc_last_checkpoint_event = timestamp;
}

fbt::sls_checkpoint:entry
{
    checkpoint_start = last_checkpoint_event = timestamp;
}

sls::slsckpt_metadata:
{
    printf("%s\t%d ns\n", stringof(arg0), timestamp - proc_last_checkpoint_event);
    proc_last_checkpoint_event = timestamp;
}

sls:::stopclock_start
{
    stopclock_start = timestamp;
}

sls:::stopclock_finish
{
    stopclock_finish = timestamp;
    printf("%s\t%d ns\n", "Application stop time", stopclock_start- stopclock_start);
}

sls::sls_checkpoint:
{
    printf("%s\t%d ns\n", stringof(arg0), timestamp - last_checkpoint_event);
    last_checkpoint_event = timestamp;
}

END
{
    printf("%s\t%d ns\n", "Total time", last_checkpoint_event - proc_checkpoint_start);
}
