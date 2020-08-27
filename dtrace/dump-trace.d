#pragma D option quiet


fbt::sls_dump:entry
{
    self->traceme = 1;

}

fbt:::
/self->traceme/
{
    trace(timestamp);

}

fbt::sls_dump:return
/self->traceme/
{
    self->traceme = 0;
    
}

