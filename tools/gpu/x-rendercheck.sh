#!/bin/sh
# Run rendercheck (X Render conformance) against a second X server on KMS +
# glamor, started on vt8 next to the desktop and stopped afterwards.
# usage (on the tablet, as root): x-rendercheck.sh [rendercheck args...]
# default: the groups glamor depends on, a8r8g8b8/x8r8g8b8/a8, Src and Over
SHIM=/opt/mesa-kbase/lib/libpanfrost_kbase_drm_shim.so
CONF=$(mktemp)
grep -v 'Option "Rotate"' /etc/X11/t510/kms.conf > "$CONF"
LD_PRELOAD=$SHIM /usr/lib/xorg/Xorg :1 vt8 -config "$CONF" -nolisten tcp -noreset \
	-logfile /tmp/Xorg.1.log > /tmp/Xorg.1.stderr 2>&1 &
XPID=$!
for i in $(seq 1 30); do [ -e /tmp/.X11-unix/X1 ] && break; sleep 0.5; done
[ $# -gt 0 ] || set -- -t fill,dcoords,scoords,mcoords,tscoords,tmcoords,blend,composite,cacomposite,gradients,repeat,triangles,bug7366 \
	-f a8r8g8b8,x8r8g8b8,a8 -o Src,Over
DISPLAY=:1 rendercheck "$@" > /tmp/rendercheck.log 2>&1
kill $XPID; wait $XPID 2>/dev/null
rm -f "$CONF"
grep -E "glamor X acceleration" /tmp/Xorg.1.log
grep -E "test failed|tests passed" /tmp/rendercheck.log
