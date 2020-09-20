# Stripe the Aurora disk
function aurstripe {
    if [ "$STRIPING" = false ];
    then
	return
    fi

    gstripe load
    gstripe stop "$STRIPENAME"
    gstripe create -s "$STRIPESIZE" -v "$STRIPENAME" $STRIPEDISKS
    DRIVE=$STRIPEDRIVE
}

# Load the SLOS, create a filesystem, and load the SLS.
function aurload {
    # Load the SLOS itself.
    kldload "$SLSDIR"/slos/slos.ko

    # Create the filesystem on disk.
    "$SLSFS" $DRIVE
    mount -rw -t slsfs $DRIVE $MOUNTDIR

    # Load and configure the SLS>
    kldload "$SLSDIR"/kmod/sls.ko

    # Set the sysctls, including SLOS checkpoints per second
    # and setting the SLS to dump asynchronously to disk.
    sysctl aurora.async_slos=1
    sysctl aurora.sync_slos=0
    sysctl aurora_slos.checkpointtime=200

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
    sleep 1

    # Make sure all data is dumped (just as a precaution)
    # NOTE: I'm not sure this does anything for the SLOS
    sync

    # Remove the SLOS
    umount "$MOUNTDIR"
    kldunload slos

    # Turn the stripe off
    gstripe stop "$STRIPENAME"
}

# Clean up any data produced by Aurora
function aurclean {
    # Clean up the output produced by Aurora
    rm -r "$OUTDIR"

    # Remove any data created for the Aurora web servers
    rm -rf "$AURWEBROOTDIR"

    # Recreate the stripe for good measure
    aurstripe
}

# Set up the SLS. Done after setting up the benchmark.
function slsckpt {
    # Check if we actually supplied a process to checkpoint
    if [ -z "$1" ]
    then
	echo "No PID of process to be checkpointed specified"
	exit 0
    fi

    # Check if we're doing delta checkpointing.
    if [ "$DELTA" == "yes" ]
    then
	DELTACONF="-d"
    else
	DELTACONF=" "
    fi

    # Possibly enable dtrace
    if [ "DTRACEENABLE" == "yes" ]
    then
    	dtrace -s "$DTRACE" > "$DTRACEFILE" &
    fi

    # Start checkpointing.
    "$SLSCTL" partadd -o "$OID" -b "$BACKEND" -t "$CKPTPERIOD" -i
    "$SLSCTL" attach -o "$OID" -p "$1"
    "$SLSCTL" checkpoint -o "$OID"
}

function slsrest {
    # Check if we need to stop the processes at the kernel at restore
    if [ "$RESTSTOP" == "yes" ]
    then
	RESTSTOPCONF="-s"
    else
	RESTSTOPCONF=""
    fi

    # Actually do the restore
    "$SLSCTL" restore -o "$OID" $RESTSTOPCONF
}
