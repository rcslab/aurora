LIGHTLOGDIRSLS="$MOUNTFILE/lighttpd_log"
LIGHTLOGDIRUSR="/usr/local/lighttpd"
LIGHT="/usr/local/sbin/lighttpd"
LIGHTCONFFILE="/usr/local/etc/lighttpd/lighttpd.conf"

LIGHTWRKCONFFILE="$OUTDIR/lighttpd_wrk.conf"
LIGHTTIME="10"
LIGHTTHREAD="5"
LIGHTCONNS="10"
LIGHTURL="http://localhost"

function listart {
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

    wrk -d "$LIGHTTIME" -t "$LIGHTTIME" -c "$LIGHTCONNS" "$LIGHTURL"
}

function listop {
    pkill lighttpd
    rm -r "$LIGHTLOGDIRUSR"
}
