# NOTE: Needs to be called _after_ aurload
function ngstart {
    # Make sure all nginx logs are in the SLOS
    rm -rf "$NGINXLOGDIRUSR" "$NGINXLOGDIRSLS"
    mkdir "$NGINXLOGDIRSLS"
    ln -s "$NGINXLOGDIRSLS" "$NGINXLOGDIRUSR"

    cd "$MOUNTDIR"
    "$NGINXBIN" -c "$NGINXCONFFILE"
    cd -
}

function ngstop {
    # Kill the master nginx process and any workers
    pkill nginx
    rm -r "$NGINXLOGDIRUSR"
}

function ngpid() {
    NGINXMAXPID=$((`sysctl -n kern.pid_max` + 1))
    NGINXPID=$NGINXMAXPID
    for ngpid in `pidof nginx`
    do
	if [ "$ngpid" -lt "$NGINXPID" ]
	then
	    NGINXPID="$ngpid"
	fi
    done

    if [ "$ngpid" -eq "$NGINXMAXPID" ]
    then
	echo "Could not find nginx pid"
    fi

}
