#!/bin/sh

PARTITIONS="$DISK GPT {auto freebsd-ufs /}"
DISTRIBUTIONS="base.txz"
BSDINSTALL_CHROOT="$MNT"

#!/bin/sh

sysrc sshd_enable="NO"

# XXX These don't work, because there is no networking at install time
pkg install -y readline
pkg install -y bash
pkg install -y python
pkg install -y redis

return
