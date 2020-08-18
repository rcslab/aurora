LIGHTLOGDIRSLS="$MOUNTFILE/lighttpd_log"
LIGHTLOGDIRUSR="/usr/local/lighttpd"
LIGHT="/usr/local/sbin/lighttpd"
LIGHTCONFFILE="$SLSDIR/scripts/lighttpd.conf"

LIGHTWRKCONFFILE="$OUTDIR/lighttpd_wrk.conf"
LIGHTTIME="10"
LIGHTTHREAD="5"
LIGHTCONNS="10"
LIGHTWRKURL="http://129.97.75.126:80/"
LIGHTWRKFILE="/bigfile"
LIGHTREMOTE="tortilla"

function listart {
    # Remove the log directory
    rm -rf "$LIGHTLOGDIRUSR" "$LIGHTLOGDIRSLS"
    mkdir "$LIGHTLOGDIRSLS"
    ln -s "$LIGHTLOGDIRSLS" "$LIGHTLOGDIRUSR"

    cd "$MOUNTFILE"
    "$LIGHT" -f "$LIGHTCONFFILE"
    cd -
}

function libench {
    echo "LIGHTTIME,$LIGHTTIME" >> "$LIGHTWRKCONFFILE"
    echo "LIGHTTHREAD,$LIGHTTHREAD" >> "$LIGHTWRKCONFFILE"
    echo "LIGHTCONNS,$LIGHTCONNS" >> "$LIGHTWRKCONFFILE"

    ssh "$LIGHTREMOTE" wrk -d "$LIGHTTIME" -t "$LIGHTTHREAD" -c "$LIGHTCONNS" "$LIGHTWRKURL/$LIGHTWRKFILE"
}

function listop {
    pkill lighttpd
    rm -r "$LIGHTLOGDIRUSR"
}
