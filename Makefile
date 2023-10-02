SUBDIR=include libsls metropolis sls slos benchmarks tests tools

IDENT=${:!sysctl -n kern.ident!}
.if (${IDENT} == "FASTDBG")
.MAKEOVERRIDES=FASTDBG
.elif (${IDENT} == "SLOWDBG")
.MAKEOVERRIDES=SLOWDBG
.elif (${IDENT} == "PERF")
.MAKEOVERRIDES=PERF
.elif (${IDENT} == "GENERIC")
.MAKEOVERRIDES=GENERIC
.else
.warning Unknown kernel ident
.endif

.include <bsd.subdir.mk>
