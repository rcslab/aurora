#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#
#
# Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
# ident	"%Z%%M%	%I%	%E% SMI"

# Single threaded asynchronous ($sync) sequential writes (1MB I/Os) to
# a 1GB file.
# Stops after 1 series of 1024 ($count) writes has been done.

set $dir=/testmnt
set $cached=false
set $iosize=4k
set $nthreads=1
set $sync=false
set $runtime=30

define file name=bigfile1,path=$dir,size=0,prealloc
define file name=bigfile2,path=$dir,size=0,prealloc
define file name=bigfile3,path=$dir,size=0,prealloc
define file name=bigfile4,path=$dir,size=0,prealloc

define process name=filewriter,instances=1
{

  thread name=filewriterthread,memsize=10m,instances=$nthreads
  {
    flowop appendfile name=write-file1,dsync=$sync,filename=bigfile1,iosize=$iosize
  }

  thread name=filewriterthread,memsize=10m,instances=$nthreads
  {
    flowop appendfile name=write-file2,dsync=$sync,filename=bigfile2,iosize=$iosize
  }

  thread name=filewriterthread,memsize=10m,instances=$nthreads
  {
    flowop appendfile name=write-file3,dsync=$sync,filename=bigfile3,iosize=$iosize
  }

  thread name=filewriterthread,memsize=10m,instances=$nthreads
  {
    flowop appendfile name=write-file4,dsync=$sync,filename=bigfile4,iosize=$iosize
  }

}

run $runtime
echo  "FileMicro-SeqWrite Version 2.2 personality successfully loaded"
