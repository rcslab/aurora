# SLS script location
SLSDIR="/root/sls/"
SLSCTL="$SLSDIR/tools/slsctl/slsctl"
SLSFS="$SLSDIR/tools/newfs/newfs"

# SLS parameters
OID="1000"
BACKEND="slos"
CKPTFREQ="5000"
DELTA="no"

# Output files
OUTDIR="./output/"
CONFFILE="$OUTDIR/aurora.conf"
DTRACEFILE="$OUTDIR/stages.csv"

# Dtrace script
DTRACE="$SCRIPTDIR/stages.d"

# SLOS setup configuration
STRIPESIZE="65536"
STRIPEDISKS="vtbd0 vtbd1 vtbd2 vtbd3"
STRIPENAME="st0"
STRIPEDRIVE="/dev/stripe/$STRIPENAME"
MOUNTFILE="/testmnt"

DRIVE="/dev/vtbd1"

function aurstripe {
    gstripe create -s "$STRIPESIZE" -v "$STRIPENAME" "$STRIPEDISKS"
    DRIVE=$STRIPEDRIVE
}

# Load the SLOS, create a filesystem, and load the SLS.
function aurload {
    # Load the SLOS itself.
    kldload "$SLSDIR"/slos/slos.ko

    # Create the filesystem on disk.
    "$SLSFS" $DRIVE
    mount -rw -t slsfs $DRIVE /testmnt

    # Load and configure the SLS>
    kldload "$SLSDIR"/kmod/sls.ko

    sysctl aurora.async_slos=1
    sysctl aurora.sync_slos=0

    # Dump the configuration to a file.
    mkdir -p "$OUTDIR"
    sysctl aurora > "$OUTDIR/aurora.sysctl"
}

# Unload the SLS and unmount the SLOS.
function aurunload {
    # Kill all tracing scripts and wait for them to finish
    pkill -SIGKILL dtrace
    sleep 1

    # Clean up all Aurora state in the system
    kldunload sls

    # Make sure all data is dumped (just as a precaution)
    # NOTE: I'm not sure this does anything for the SLOS
    sync

    # Remove the SLOS
    umount "$MOUNTFILE"
    kldunload slos
}

# Set up the SLS. Done after setting up the benchmark.
function slsckpt {
    # Check if we're doing delta checkpointing.
    if [ "$DELTA" == "yes" ]
    then
	DELTACONF="-d"
    else
	DELTACONF=""
    fi

    echo "BACKEND,$BACKEND" >> "$CONFFILE"
    echo "CKPTFREQ,$CKPTFREQ" >> "$CONFFILE"
    echo "DELTA,$DELTA" >> "$CONFFILE"

    #dtrace -s "$DTRACE" > "$DTRACEFILE" &

    # Start checkpointing.
    "$SLSCTL" partadd -o "$OID" -b "$BACKEND" -t "$CKPTFREQ"
    "$SLSCTL" attach -o "$OID" -p "$1"
    "$SLSCTL" checkpoint -o "$OID"
}

function slsrest {
    "$SLSCTL" restore -o "$OID"
}

