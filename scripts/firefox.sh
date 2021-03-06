# Hollow out the python script, turn it into just the geckodriver setup/launch
function ffstart() {
    cd "$JSBENCHMARKPATH"
    $(python -m http.server) &
    cd -
}

function ffbench() {
    cd "$MOUNTDIR"
    python "$FIREFOXDIR/benchmark.py"
}

function ffstop() {
    pkill python
    # XXX Why are there still marionette instances?
    pkill firefox
    pkill geckodriver
}
