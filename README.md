Aurora Single Level Store
=========================

The Aurora Single Level Store provides transparent persistence for applications 
based on lightning fast incremental checkpoints.  Aurora checkpoints are either 
stored on-disk in a custom object store or in memory for debugging 
applications.  Unlike older single level stores Aurora provides a file system 
namespace into the store to enable backwards compatibility.

Applications can use the single level store API provided libsls to achieve 
higher performance and control Aurora's behavior. 

The file tree is the following:

| Directories	| Contents						    |
|---------------|-----------------------------------------------------------|
| benchmarks/	| Performance benchmarks				    |
| dtrace/	| DTrace scripts for debugging and performance measurements |
| include/	| Headers for userspace and the kernel			    |
| sls/		| Single Level Store in memory module			    |
| libsls/	| Userspace library implementing the API		    |
| scripts/	| Bash utilities for using Aurora and performance benchmarks|
| slfs/		| VFS filesystem for using files with Aurora		    |
| slos/		| Object Store backing Aurora				    |
| tests/	| Correctness tests for Aurora				    |
| tools/	| Userspace tools					    |

Installation
------------

Dependencies:
 - FreeBSD 12.3 with Aurora patches
 - pidof (testbench)

Before you begin you must install and configure FreeBSD 12.3 with the Aurora 
enabling patches that can be done from our tree or a provided iso image.  The 
build depends on header changes to FreeBSD and one of four provided kernel 
configuration files: GENERIC, PERF, FASTDBG, SLOWDBG.

WARNING: If you are building FreeBSD 12.3 with our patches please remember to 
do so on top of an existing FreeBSD 12.3 system.

```
# make
```

Once complete you may install Aurora using the following commands.  Remember 
that the newly installed commands `newfs_sls` and `slsctl` will not be in your 
path until you type `rehash`.

```
# mkdir -p /usr/lib/debug/boot/modules
# mkdir -p /usr/lib/debug/sbin
# mkdir -p /usr/aurora/tests
# make install
```

The basic test suite can be run to verify things are working:

```
# cd tests
# ./testbench
```

Please note that testbenches are prefixed with project names.  Disabled 
testbenches are often from other projects.

Getting Started
---------------

First you will want to load the kernel modules `sls.ko` and `slos.ko` that 
provide the single level store and object store/file system functionality.  
This will recursively load all dependent kernel modules.

```
# kldload sls
```

Next you will want to create a SLS object store on a flash storage device, we 
use `/dev/nvd0` the first NVMe disk as an example.  You may specify any disk or 
partition available to you.

WARNING: This will destroy all data on the device /dev/nvd0.

```
# newfs_sls /dev/nvd0
```

Finally we can mount the file system in a directory `/aurora` for our example.  
This is an important step as this both mounts the file system and opens the 
object store for use by the single level store.  At the moment you may only 
have a single aurora file system attached.

```
# mkdir /aurora
# mount -t slfs /dev/nvd0 /aurora
```

To manage the single level store you can use the `slsctl` command to attach 
processes and create manual checkpoints.  The system actually lets you create 
partitions that are a logical grouping of persistent processes that can be 
either memory backed or storage backed see the `-m` flag and `-o` flag.  We 
will only cover the most basic usage here.

To make an existing process persistent you use the `attach` command.
```
# slsctl attach -p <Process PID>
```

You can manually checkpoint the application using `checkpoint`.  The `-r` flag 
will recursively checkpoint all children of the parent process.

```
slsctl checkpoint [-r]
```

You can restore a process after a system crash using the `restore` command.

```
slsctl restore
```

Publications
------------

 - Emil Tsalapatis, Ryan Hancock, Tavian Barnes, Ali José Mashtizadeh.
   The Aurora Operating System: Revisiting the Single Level Store.
   In Proceedings of the 18th Workshop on Hot Topics in Operating Systems 
   (HotOS ‘21). June, 2021.
 - Emil Tsalapatis, Ryan Hancock, Tavian Barnes, Ali José Mashtizadeh.
   The Aurora Single Level Store Operating System
   In Proceedings of the 28th ACM Symposium on Operating Systems Principles 
   (SOSP ‘21). September, 2021.

