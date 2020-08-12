SUBDIR=libsls slos tests tools shim

IDENT=${:!sysctl -n kern.ident!}
.if (${IDENT} == "FASTDBG")
.MAKEOVERRIDES=FASTDBG
.elif (${IDENT} == "SLOWDBG")
.MAKEOVERRIDES=SLOWDBG
.elif (${IDENT} == "PERF")
.MAKEOVERRIDES=PERF
.else
.warning Unknown kernel ident
.endif

.include <bsd.subdir.mk>
