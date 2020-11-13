OVERVIEW
========

This is the repository for the Aurora Single Level Store. The Single Level 
Store transparently provides persistence to applications at a millisecond 
granularity. Apps can call into Aurora to directly control the checkpointing 
process.

The file tree is the following:

| Directories	| Contents						    |
| :-----------: | :-------------------------------------------------------: |
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

USAGE
=====

The checkpoint restore cycle needs 4 commands, all from the slsctl utility:

slsctl partadd: Create an empty partition. Partitions are groups into which we enter processes,
and allow us to checkpoint groups of processes all at once. Each partition has a unique object ID
(oid), which can take any value as long as it's unique.

Options:

-o: Assign a unique OID to the partition
-d: Use if using delta instead of full checkpointing
-t: Checkpointing period. Checkpointing doesn't start immediately, but when it does, it will be
	continuous if this flag is set. Otherwise it is going to be a one-off checkpoint.
-b: Set backend. Valid values are slos for the SLOS and memory for in-memory checkpoints.

slsctl partadd -o <oid>  [-d] [-t <interval>] -b <backend>


slsctl attach: Add a process to the partition. We only need the PID of the process and the OID of the partition.
slsctl attach -o <oid> -p <pid>


slsctl checkpoint: Take a checkpoint of the partition. If the -r option is specified, even children of processes
in the partition that have not been explicitly entered into it will be checkpointed. This is useful for processes
that often spawn a lot of transient children.

slsctl checkpoint -o <oid> [-r]

slsctl restore: Restore a partition with the specified OID. The -d option spawns the partition as a daemon,
detached from all terminals.

slsctl restore -o <oid> [-d]

An example: Suppose we have a process with PID 123 that we want to checkpoint to disk only once, then restore.
We would have to run the following (example OID is 3):

partadd -o 3 -b slos
attach -o 3 -p 123
checkpoint -o 3
restore -o 3

