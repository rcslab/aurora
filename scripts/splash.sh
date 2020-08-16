SPLASHDIR="$SLSBENCHDIR/splash/codes/"
SPLASHAPPBENCHES="barnes fmm water-nsquared water-spatial"

function spbench() {
    # Some benchmarks need to have their input piped to them
    for splashbench in $SPLASHAPPBENCHES
    do
	SPBENCHDIR="$SPLASHDIR/apps/$splashbench/"
	cd "$SPBENCHDIR"
	time "$SPBENCHDIR/${splashbench^^}" < "$SPBENCHDIR/inputs/aurora"
    done

    # Some benchmarks take command line options instead of an input file.  
    # Hardcode the configuration here instead of on the file.
    cd "$SPLASHDIR/apps/"
    "$SPLASHDIR/apps/radiosity/RADIOSITY" \
	-ae 5000 -bf 0.0005 -en 0.0005 -largeroom -batch
    "$SPLASHDIR/apps/ocean/contiguous_partitions/OCEAN" -n4098

    "$SPLASHDIR/kernels/lu/contiguous_blocks/LU" -n4096
    "$SPLASHDIR/kernels/radix/RADIX" -n67108864

    cd "$SLSDIR"
}
