#pragma D option quiet

BEGIN 
{
    reads = 0;
    writes = 0;
    cmds = 0;
}

fbt::fbtree_insert:entry,
fbt::fbtree_remove:entry,
fbt::fbtree_get:entry,
fbt::fbtree_replace:entry,
fbt::slsfs_read:entry,
fbt::slsfs_write:entry,
fbt::fnode_init:entry,
fbt::fnode_keymin:entry,
fbt::slsfs_retrieve_buf:entry,
fbt::slsfs_bcreate:entry,
fbt::slsfs_bread:entry,
fbt::slsfs_bdirty:entry,
fbt::slsfs_lookupbln:entry
{
    current[probefunc] = timestamp;
    this->traceme = 1;
}

io:::start
/this->traceme && args[0] != NULL/
{
    @disk[args[1]->device_name, args[1]->unit_number, args[0]->bio_cmd] = count();
}

fbt::fbtree_insert:return,
fbt::fbtree_remove:return,
fbt::fbtree_get:return,
fbt::fbtree_replace:return,
fbt::slsfs_read:return,
fbt::slsfs_write:return,
fbt::fnode_init:return,
fbt::fnode_keymin:return,
fbt::slsfs_retrieve_buf:return,
fbt::slsfs_bcreate:return,
fbt::slsfs_bread:return,
fbt::slsfs_bdirty:return,
fbt::slsfs_lookupbln:return
{
    @ts[probefunc] = avg(timestamp - current[probefunc]);
    @counts[probefunc] = count();
    @total[probefunc] = sum(timestamp - current[probefunc]);
    this->traceme = 0;
}

END 
{
    printa(@ts);
    printf("Counts\n");
    printa(@counts);
    printf("Sums\n");
    printa(@total);
    printa(@disk);
}
