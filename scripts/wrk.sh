function wrkbench {
    ssh -p "$WRKPORT" -i "$WRKSSH" "$WRKUSER@$WRKREMOTE" wrk -d "$WRKTIME" -t "$WRKTHREAD" -c "$WRKCONNS" "$WRKURL/$WRKFILE"
}
