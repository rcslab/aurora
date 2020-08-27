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
fbt::fbtree_keymin_iter:entry,
fbt::getblk:entry,
fbt::fnode_follow:entry,
fbt::fnode_init:entry
{
    current[probefunc, tid] = timestamp;
    this->traceme = 1;
}

fbt::fbtree_insert:return,
fbt::fbtree_remove:return,
fbt::fbtree_get:return,
fbt::fbtree_replace:return,
fbt::fbtree_keymin_iter:return,
fbt::getblk:return,
fbt::fnode_follow:return,
fbt::fnode_init:return
{
    @ts[probefunc, tid] = avg(timestamp - current[probefunc, tid]);
    @counts[probefunc, tid] = count();
    @total[probefunc, tid] = sum(timestamp - current[probefunc, tid]);
    this->traceme = 0;
}

END 
{
    printa(@ts);
    printf("Counts\n");
    printa(@counts);
    printf("Sums\n");
    printa(@total);
}
