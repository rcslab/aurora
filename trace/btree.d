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
fbt::getblk:entry,
fbt::fnode_init:entry
/*
fbt::slsfs_read:entry,
fbt::fnode_keymin:entry,
fbt::slsfs_retrieve_buf:entry,
fbt::slsfs_bcreate:entry,
fbt::slsfs_bread:entry,
fbt::fnode_insert:entry,
fbt::slsfs_strategy:entry,
fbt::vfs_bio_clrbuf:entry,
fbt::fbtree_keymin_iter:entry,
fbt::uiomove_faultflag:entry,
fbt::fnode_init:entry,
fbt::bufwrite:entry,
fbt::slsfs_lookupbln:entry,
fbt::slsfs_read:entry,
fbt::fnode_init:entry,
fbt::slsfs_write:entry
*/
{
    current[probefunc] = timestamp;
    this->traceme = 1;
}

/*
io:::start
/this->traceme && args[0] != NULL/
{
    @disk[args[1]->device_name, args[1]->unit_number, args[0]->bio_cmd] = count();
}
*/

fbt::fbtree_insert:return,
fbt::fbtree_remove:return,
fbt::fbtree_get:return,
fbt::fbtree_replace:return,
fbt::getblk:return,
fbt::fnode_init:return
/*
fbt::slsfs_read:return,
fbt::slsfs_write:return,
fbt::fnode_init:return,
fbt::fnode_keymin:return,
fbt::slsfs_retrieve_buf:return,
fbt::slsfs_bcreate:return,
fbt::slsfs_bread:return,
fbt::fnode_insert:return,
fbt::vfs_bio_clrbuf:return,
fbt::slsfs_strategy:return,
fbt::bufwrite:return,
fbt::fbtree_keymin_iter:return,
fbt::uiomove_faultflag:return,
fbt::slsfs_lookupbln:return,
fbt::slsfs_read:return,
fbt::fnode_init:return,
fbt::slsfs_write:return
*/
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
/*    printa(@disk); */
}
