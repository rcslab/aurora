function listart {
    # Remove the log directory
    rm -rf "$LIGHTLOGDIRUSR" "$LIGHTLOGDIRSLS"
    mkdir "$LIGHTLOGDIRSLS"
    ln -s "$LIGHTLOGDIRSLS" "$LIGHTLOGDIRUSR"

    cd "$MOUNTDIR"
    "$LIGHT" -f "$LIGHTCONFFILE"
    cd -
}

function listop {
    pkill lighttpd
    rm -r "$LIGHTLOGDIRUSR"
}
