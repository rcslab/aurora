#!/bin/sh

# Create an Aurora root from a base.txz.

# Get the distribution mirror for the machine.
get_distribution_mirror()
{
    MIRROR="ftp://ftp.freebsd.org"

    _UNAME_R=`uname -r`
    case _UNAME_R=${_UNAME_R%-p*} in
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

if [ -z "$SRCROOT" ]; then
    echo "No SLS source directory specified"
    exit 1
fi

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

install_one_package()
{
    MNTROOT=$1
    PKG=$2
    
    CMD="pkg install -y $2"
    run_chroot_cmd $MNTROOT "${CMD}"
}

install_packages()
{
    MNTROOT=$1

    install_one_package $MNTROOT bash
    install_one_package $MNTROOT python
}

copy_dns_certs()
{
	MNTROOT=$1
	CERTS="/etc/ssl/cert.pem"
	LOCALCERTS="/usr/local/share/certs"

	cp -r $CERTS "$MNTROOT/$CERTS"
	cp -r $LOCALCERTS "$MNTROOT/$LOCALCERTS"
}

create_image()
{
    BSDINSTALL_DISTSITE=`get_distribution_mirror`
    BSDINSTALL_DISTDIR="/usr/freebsd-dist"
    BASE=`fetch_base $BSDINSTALL_DISTDIR`

    MNTROOT="/mnt"
    IP=$1
    REPO=$2

    unpack_base $MNTROOT $BASE

    mount_pseudofs $MNTROOT

    configure_dns $MNTROOT $IP
    copy_dns_certs $MNTROOT 
    configure_pkg $MNTROOT $REPO
    install_packages $MNTROOT

    umount_pseudofs $MNTROOT

    # Create the image, cleanup, and unmount.
    SLSROOT="$SRCROOT/root.tar"
    pack_base $MNTROOT $SLSROOT
}

if [ ! -z $SLSROOT ]; then
    echo "No root specified"
    exit 0;
fi

if [ -z $IP ]; then
	echo "No DNS server specified, using 8.8.8.8"
	IP="8.8.8.8"
fi

if [ -z $REPO ]; then
	echo "No Aurora package repo specified, using RCS"
	REPO="https://rcs.uwaterloo.ca/~ali/FreeBSD:amd64:12.1/"
fi

create_image $IP $REPO
