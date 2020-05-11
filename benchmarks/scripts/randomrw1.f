set $dir=/testmnt
set $filesize=2g
set $iosize=64k
set $nthreads=1
set $workingset=0
set $directio=0
set $runtime=30

define file name=largefile1,path=$dir,size=$filesize,prealloc,reuse,paralloc

define process name=rand-rw1,instances=1
{
  thread name=thwr1,memsize=5m,instances=$nthreads
  {
    flowop write name=rw1,filename=largefile1,iosize=$iosize,random,workingset=$workingset,directio=$directio
  }
}

run $runtime

echo "Random RW Version 3.0 personality successfully loaded"
