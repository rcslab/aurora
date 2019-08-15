#pragma D option quiet

BEGIN 
{
    reads = 0;
    writes = 0;
    cmds = 0;
}

fbt::btree_add:entry,
fbt::btree_delete:entry,
fbt::btree_candidate:entry,
fbt::btree_search:entry,
fbt::bnode_write:entry,
fbt::bnode_child:entry,
fbt::bnode_read:entry,
fbt::btree_keymin:entry,
fbt::btree_keymax:entry,
fbt::bnode_search:entry,
fbt::bnode_free:entry,
fbt::slos_readblk:entry
{
    current[probefunc] = timestamp;
    this->traceme = 1;
}

io:::start
/this->traceme && args[0] != NULL/
{
    @disk[args[1]->device_name, args[1]->unit_number, args[0]->bio_cmd] = count();
}

fbt::btree_add:return,
fbt::btree_delete:return,
fbt::btree_candidate:return,
fbt::btree_search:return,
fbt::bnode_write:return,
fbt::bnode_read:return,
fbt::bnode_child:return,
fbt::btree_keymin:return,
fbt::btree_keymax:return,
fbt::bnode_search:return,
fbt::bnode_free:return,
fbt::slos_readblk:return
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
