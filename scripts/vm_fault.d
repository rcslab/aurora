#!/usr/sbin/dtrace -s

#pragma D option quiet

fbt::vm_fault_hold:entry
/self->fault == 0/
{
    self->fault = timestamp;
}

fbt::vm_fault_hold:return
/self->fault/
{
    @faults[pid] = quantize(timestamp - self->fault);
    self->fault = 0;
}

END
{
    printa(@faults);
}
