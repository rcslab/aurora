TESTING
=======

Testing is done by running the `testbench` script from this directory. The 
script creates a memory disk and flashes the filesystem into it. Every test 
starts with a fresh filesystem and module. Assuming tests do not corrupt kernel 
state and properly signal failure, we can then one feature at a time and treat 
it as a given for all tests after that.

The tests are number XYZ, where X is the category of the test, and YZ is the 
test number, The categories are as follows:

| Code	| Category						|
| :---: | :---------------------------------------------------: |
| 0	| Basic module insertion and removal			|
| 1	| SLFS filesystem					|
| 2	| SLS checkpointing					|
| 3	| SLOS object store					|
| 4	| Macro workloads					|
| 5	| Aurora integrated workloads API usage			|
