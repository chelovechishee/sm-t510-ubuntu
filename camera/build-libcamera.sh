#!/bin/bash
# libcamera 0.7.0-1ubuntu2 with the Exynos 7885 CSIS in the simple pipeline
# (software ISP) and the S5K4HA sensor: cross-built for arm64, then put into
# Ubuntu's own libcamera0.7 and libcamera-ipa debs as 0.7.0-1ubuntu2+t510.N.
# Only the simple and uvcvideo pipelines and the simple IPA are built; the
# other packages (cam, gstreamer, pipewire) keep Ubuntu's builds, same ABI.
# Sources: Ubuntu libcamera_0.7.0-1ubuntu2 unpacked with dpkg-source into
# $B/src/libcamera plus camera/libcamera-0.7.0-t510.patch, and the arm64
# libcamera0.7 and libcamera-ipa debs of that version in $B.
# Needs: libyaml-dev:arm64 libssl-dev:arm64 libudev-dev:arm64 python3-ply.
set -euo pipefail
T=$(cd "$(dirname "$0")/.." && pwd)
B=${LIBCAMERA_BUILD:-$HOME/build/libcamera-t510}
S=$B/src/libcamera
UV=0.7.0-1ubuntu2
V=${UV}+t510.${REV:-1}
MA=aarch64-linux-gnu

cat > "$B/cross-arm64.ini" <<EOI
[binaries]
c = 'aarch64-linux-gnu-gcc'
cpp = 'aarch64-linux-gnu-g++'
ar = 'aarch64-linux-gnu-ar'
strip = 'aarch64-linux-gnu-strip'
pkg-config = 'pkg-config'

[properties]
pkg_config_libdir = '/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig'

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOI

rm -rf "$B/build" "$B/inst"
meson setup "$B/build" "$S" --cross-file "$B/cross-arm64.ini" \
	--prefix /usr --libdir lib/$MA --libexecdir libexec/$MA \
	-Dbuildtype=release -Dwerror=false \
	-Dpipelines=simple,uvcvideo -Dipas=simple \
	-Dcam=disabled -Dqcam=disabled -Dgstreamer=disabled -Dpycamera=disabled \
	-Dv4l2=false -Dlc-compliance=disabled -Ddocumentation=disabled \
	-Dtest=false -Dtracing=disabled -Dlibunwind=disabled
ninja -C "$B/build"
DESTDIR="$B/inst" ninja -C "$B/build" install

# Strip, then sign the IPA with this build's key, as debian/rules does
find "$B/inst/usr/lib/$MA" -type f -name '*.so*' ! -name '*.sign' -exec aarch64-linux-gnu-strip --strip-unneeded {} +
find "$B/inst/usr/libexec/$MA" -type f -exec aarch64-linux-gnu-strip --strip-unneeded {} +
MESON_INSTALL_DESTDIR_PREFIX="$B/inst/usr" "$S/src/ipa/ipa-sign-install.sh" \
	"$B/build/src/ipa-priv-key.pem" "lib/$MA/libcamera/ipa/ipa_soft_simple.so"

# Repack Ubuntu's debs with these files
O=$T/out/debs
mkdir -p "$O"
# Depends of this build: OpenSSL instead of GnuTLS, no tracing, no libdw,
# no libpisp/libyuv (Raspberry Pi pipeline), EGL/GLES for the GPU debayer
DEP_LIB="libc6 (>= 2.38), libegl1, libgcc-s1 (>= 3.3.1), libgles2, libssl3t64 (>= 3.0.0), libstdc++6 (>= 14), libudev1 (>= 183), libyaml-0-2"
DEP_IPA="libc6 (>= 2.38), libcamera0.7 (>= $V), libgcc-s1 (>= 3.3.1), libstdc++6 (>= 14)"

repack() {
	local pkg=$1 dep=$2 w=$B/repack/$1
	shift 2
	rm -rf "$w"
	mkdir -p "$B/repack"
	dpkg-deb -R "$B/${pkg}_${UV}_arm64.deb" "$w"
	"$@"
	sed -i "s/^Version: .*/Version: $V/; s/^Depends: .*/Depends: $dep/" "$w/DEBIAN/control"
	(cd "$w" && find . -path ./DEBIAN -prune -o -type f -printf '%P\0' | sort -z | xargs -0 md5sum > DEBIAN/md5sums)
	dpkg-deb --root-owner-group -b "$w" "$O/${pkg}_${V}_arm64.deb"
}
lib() {
	local w=$B/repack/libcamera0.7
	rm -f "$w"/usr/lib/$MA/libcamera*.so.*
	cp -a "$B"/inst/usr/lib/$MA/libcamera*.so.* "$w/usr/lib/$MA/"
}
ipa() {
	local w=$B/repack/libcamera-ipa
	rm -rf "$w/usr/lib/$MA/libcamera/ipa" "$w/usr/libexec/$MA/libcamera" "$w/usr/share/libcamera"
	cp -a "$B/inst/usr/lib/$MA/libcamera/ipa" "$w/usr/lib/$MA/libcamera/"
	cp -a "$B/inst/usr/libexec/$MA/libcamera" "$w/usr/libexec/$MA/"
	cp -a "$B/inst/usr/share/libcamera" "$w/usr/share/"
}
repack libcamera0.7 "$DEP_LIB" lib
repack libcamera-ipa "$DEP_IPA" ipa
ls -l "$O"/libcamera*_${V}_arm64.deb
echo LIBCAMERA_DONE
