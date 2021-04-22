#!/usr/sbin/dtrace -s

fbt::trap_pfault:entry
/pid == $1/
{
	self->t = timestamp;
}

fbt::trap_pfault:return
/pid == $1 && self->t != 0/
{
	@num[tid] = count();
	@total[tid] = sum(timestamp - self->t);
	self->t = 0;
}

END
{
	printf("\n%10s %20s\n", "TID", "Time (ns)");
	printa("%10d %@20u\n", @total);
	printf("\n%10s %20s\n", "TID", "Faults");
	printa("%10d %@20u\n", @num);
}

