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

sls:sls::fileckptstart
{
	self->tstart = timestamp;
}

sls:sls::fileckptend
{
	@time[ftype[arg0]] = avg(timestamp - self->tstart)
}

sls:sls::fileckptend
{
	@time["err"] = avg(timestamp - self->tstart)
}

sls:sls::sysvstart
{
	self->tstart = timestamp;
}

sls:sls::sysvend
{
	@time["sysvshm"] = avg(timestamp - self->tstart);
}


sls:sls::sysverror
{
	@time["err"] = avg(timestamp - self->tstart);
}

sls:sls::sysverror
{
	@time["err"] = avg(timestamp - self->tstart);
}

sls:sls::namestart
{
	self->nstart = timestamp;
}

sls:sls::nameend
{
	@time["name"] = avg(timestamp - self->nstart); 
}

sls:sls::nameerr
{
	@time["nameerr"] = avg(timestamp - self->nstart); 
}

END
{
}
