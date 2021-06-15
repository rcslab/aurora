#!/usr/sbin/dtrace -s

#pragma D option quiet
int tstart, nstart;

BEGIN
{
	ftype[1] = "vnode";
	ftype[2] = "socket";
	ftype[3] = "pipe";
	ftype[5] = "kqueue";
	ftype[8] = "posixshm";
	ftype[10] = "pts";
}

sls:::fileprobe_start
{
	self->tstart = timestamp;
}

sls:::fileprobe_return
{
	@ckpt[ftype[arg0]] = avg(timestamp - self->tstart);
}

fbt::slsckpt_sysvshm:entry
{
	self->tstart = timestamp;
}

fbt::slsckpt_sysvshm:return
{
	@ckpt["sysvshm"] = avg(timestamp - self->tstart);
}

sls:::filerest_start
{
	self->tstart = timestamp;
}

sls:::filerest_return
{
	@rest[ftype[arg0]] = avg(timestamp - self->tstart);
}

fbt::slsrest_sysvshm:entry
{
	self->tstart = timestamp;
}

fbt::slsrest_sysvshm:return
{
	@rest["sysvshm"] = avg(timestamp - self->tstart);
}

END
{
	printf("Checkpoint times (ns):");
	printa(@ckpt);
	printf("Restore times (ns):");
	printa(@rest);
}
