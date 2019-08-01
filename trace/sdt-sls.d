BEGIN {
	ckpt_stop_time = 0;
	ckpt_cont_time_total = 0;	
	ckpt_stop_count = 0;
	ckpt_cont_count = 0;
	wake = 0;
	wake_total = 0;
	wake_count = 0;
}

sls:::stop_entry
{
	stop_time = timestamp;
}

sls:::stop_exit
{
	ckpt_stop_time += timestamp - stop_time;
	ckpt_stop_count += 1;
	stop_time = timestamp;
	sig = 1;
	ckpt_cont_time = timestamp;
}

END 
{
	TO_MILI = 1000000;
	printf("%s: %d, %d, %d\n", "Chkpt Stop", 
			ckpt_stop_time, ckpt_stop_count,
			ckpt_stop_time / (TO_MILI * ckpt_stop_count));
}
