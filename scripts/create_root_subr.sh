#!/bin/sh

# Subroutines for root creation.

# Get the distribution mirror for the machine.
get_distribution_mirror()
{
    MIRROR="ftp://ftp.freebsd.org"

    _UNAME_R=`uname -r`
    _UNAME_R=${_UNAME_R%-p*} 
    case $_UNAME_R in
    *-ALPHA*|*-CURRENT|*-STABLE|*-PRERELEASE)
	RELDIR="snapshots"
	;;
    *)
	RELDIR="releases"
	;;
    esac
    
    echo "$MIRROR/pub/FreeBSD/${RELDIR}/`uname -m`/`uname -p`/${_UNAME_R}"
}

# Fetch the base from the mirror if necessary.
fetch_base()
{
    BSDINSTALL_DISTDIR=$1
    BSDINSTALL_DISTSITE=`get_distribution_mirror`
    DISTRIBUTIONS="base.txz"
    DISTPATH="$BSDINSTALL_DISTDIR/$DISTRIBUTIONS"

    mkdir -p $BSDINSTALL_DISTDIR

    # Get the sources if needed. Based on mirrorselect.
    if [ ! -f $DISTPATH ]; then
	# Export the parameters to the bsdinstall command.
	export BSDINSTALL_DISTSITE
	export BSDINSTALL_DISTDIR
	export DISTRIBUTIONS
	bsdinstall distfetch

	if [ $? -ne 0 ]; then
	    echo "Failed to fetch $DISTRIBUTIONS from $DISTSITE"
	    exit 1
	fi
    fi

    echo $DISTPATH
}

unpack_base()
{
    MNTROOT=$1
    BASE=$2

    tar -C $MNTROOT -xf $BASE
}

pack_base()
{
    MNTROOT=$1
    SLSROOT=$2

    tar -C "$MNTROOT" -cf "$SLSROOT" .

}

run_chroot_cmd()
{
    MNTROOT=$1
    CMD=$2

    chroot $MNTROOT /bin/sh -c "${CMD}"
}

configure_dns()
{
    MNTROOT=$1
    IP=$2

    DNS_SERVERS="nameserver $IP"
    CMD="echo ${DNS_SERVERS} > /etc/resolv.conf"

    run_chroot_cmd $MNTROOT "${CMD}"
}

configure_pkg()
{
    MNTROOT=$1 
    REPO=$2
    PKGCONF="FreeBSD: { \
url: \"$REPO\", \
signature_type: \"none\", \
fingerprints: \"usr/share/keys/pkg\", \
enabled: yes \
}"
    CMD="echo ${PKGCONF} > /etc/pkg/FreeBSD.conf"

    run_chroot_cmd $MNTROOT "${CMD}"
}

mount_pseudofs()
{
    MNTROOT=$1

    mount -t devfs devfs $MNTROOT/dev
    mount -t procfs procfs $MNTROOT/proc
}

umount_pseudofs()
{
    MNTROOT=$1

    umount $MNTROOT/dev
    umount $MNTROOT/proc
}

install_package()
{
    MNTROOT=$1
    PKG=$2
    
    CMD="pkg install -y $2"
    run_chroot_cmd $MNTROOT "${CMD}"
}

copy_certs()
{
	MNTROOT=$1
	CERTS="/etc/ssl/cert.pem"
	LOCALCERTS="/usr/local/share/certs"

	cp -r $CERTS "$MNTROOT/$CERTS"
	cp -r $LOCALCERTS "$MNTROOT/$LOCALCERTS"
}
