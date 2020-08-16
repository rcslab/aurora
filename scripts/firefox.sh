FIREFOXDIR="$SLSBENCHDIR/firefox/"
FIREFOXSCRIPT="$FIREFOXDIR/run.sh"
JSBENCHMARKPATH="$FIREFOXDIR/hosted"

# Hollow out the python script, turn it into just the geckodriver setup/launch
function ffstart() {
    cd "$JSBENCHMARKPATH"
    $(python -m http.server) &
    cd -
}

function ffbench() {
    cd "$MOUNTFILE"
    python "$FIREFOXDIR/benchmark.py"
}

function ffstop() {
    pkill python
    # XXX Why are there still marionette instances?
    pkill firefox
    pkill geckodriver
}
