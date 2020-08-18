NGINX="$SLSBENCHDIR/nginx/objs/nginx"
NGINXLOGDIRSLS="$MOUNTFILE/nginx_logs"
NGINXLOGDIRUSR="/usr/local/nginx/logs"

NGINXTIME="10"
NGINXTHREAD="5"
NGINXCONNS="10"
NGINXWRKCONFFILE="$OUTDIR/nginx_wrk.conf"
NGINXWRKFILE="$OUTDIR/nginx_wrk.results"
NGINXURL="http://129.97.75.126:80/"
NGINXREMOTEURL="tortilla"
NGINXFILE="/bigfile"
#NGINXCONFFILE="$SLSDIR/scripts/nginx.conf"
NGINXCONFFILE="/usr/local/nginx/conf/nginx.conf"

NGINXMAXPID="65536"
NGINXPID="$NGINXMAXPID"

# NOTE: Needs to be called _after_ aurload
function ngstart {
    # Make sure all nginx logs are in the SLOS
    rm -rf "$NGINXLOGDIRUSR" "$NGINXLOGDIRSLS"
    mkdir "$NGINXLOGDIRSLS"
    ln -s "$NGINXLOGDIRSLS" "$NGINXLOGDIRUSR"

    cd "$MOUNTFILE"
    "$NGINX" -c "$SNGINXCONFFILE"
    cd -
}

function ngbench {
    echo "NGINXTIME,$NGINXTIME" >> "$NGINXWRKCONFFILE"
    echo "NGINXTHREAD,$NGINXTHREAD" >> "$NGINXWRKCONFFILE"
    echo "NGINXCONNS,$NGINXCONNS" >> "$NGINXWRKCONFFILE"

    ssh "$NGINXREMOTEURL" wrk -d "$NGINXTIME" -t "$NGINXTIME" -c "$NGINXCONNS" "$NGINXURL/$NGINXFILE"
}

function ngstop {
    # Kill the master nginx process and any workers
    pkill nginx
    rm -r "$NGINXLOGDIRUSR"
}

function ngpid() {
    NGINXPID="$NGINXMAXPID"
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
