#pragma D option quiet
int restore_start, last_restore_event;
int proc_restore_start, proc_last_restore_event;

BEGIN
{
}

fbt::slsrest_metadata:entry
{
    proc_restore_start = proc_last_restore_event = timestamp;
}

fbt::sls_rest:entry
{
    restore_start = last_restore_event = timestamp;
}

sls::slsrest_metadata:
{
    printf("%s\t%d ns\n", stringof(arg0), timestamp - proc_last_restore_event);
    proc_last_restore_event = timestamp;
}

sls::sls_rest:
{
    printf("%s\t%d ns\n", stringof(arg0), timestamp - last_restore_event);
    last_restore_event = timestamp;
}

END
{
    printf("%s\t%d ns\n", "Total time", last_restore_event - restore_start);
}
