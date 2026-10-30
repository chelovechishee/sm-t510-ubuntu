#!/bin/bash
# Mesa 26.0.8-1ubuntu0.3 with the G71 CLPER_OLD fix: native CL tools, then
# libgallium for arm64 (Panfrost + softpipe, no LLVM) with the same version
# string as Ubuntu's, so the stock libEGL/libGLX/libgbm load it.
set -euo pipefail
# Sources: Ubuntu mesa_26.0.8-1ubuntu0.3 (archive.ubuntu.com pool) unpacked with
# dpkg-source into $B/src/mesa, VERSION = 26.0.8-1ubuntu0.3, plus
# mesa-26.0.8-g71-zero-derivatives.patch. Needs the arm64 -dev packages listed
# in mainline/README.md.
B=${MESA_BUILD:-$HOME/build/mesa-t510}
S=$B/src/mesa
cd "$S"

if [ ! -x "$B/native-inst/bin/mesa_clc" ]; then
	[ -d "$B/build-native" ] || meson setup "$B/build-native" --prefix "$B/native-inst" \
		-Dbuildtype=release -Dllvm=enabled \
		-Dgallium-drivers=panfrost -Dvulkan-drivers= -Dplatforms= \
		-Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles2=disabled \
		-Dmesa-clc=enabled -Dinstall-mesa-clc=true \
		-Dprecomp-compiler=enabled -Dinstall-precomp-compiler=true
	ninja -C "$B/build-native"
	ninja -C "$B/build-native" install
fi

cat > "$B/cross-arm64.ini" <<EOF
[binaries]
c = 'aarch64-linux-gnu-gcc'
cpp = 'aarch64-linux-gnu-g++'
ar = 'aarch64-linux-gnu-ar'
strip = 'aarch64-linux-gnu-strip'
pkg-config = 'pkg-config'
mesa_clc = '$B/native-inst/bin/mesa_clc'
vtn_bindgen2 = '$B/native-inst/bin/vtn_bindgen2'
panfrost_compile = '$B/native-inst/bin/panfrost_compile'

[properties]
pkg_config_libdir = '/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig'

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'cortex-a73'
endian = 'little'
EOF

export PATH="$B/native-inst/bin:$PATH"
rm -rf "$B/build-arm64.failed"
[ -d "$B/build-arm64" ] || meson setup "$B/build-arm64" --cross-file "$B/cross-arm64.ini" \
	--prefix /usr --libdir lib/aarch64-linux-gnu \
	-Dbuildtype=release -Db_ndebug=true -Dllvm=disabled \
	-Ddri-drivers-path=/usr/lib/aarch64-linux-gnu/dri \
	-Dgallium-drivers=panfrost,softpipe -Dvulkan-drivers= \
	-Dplatforms=x11,wayland -Dglvnd=enabled -Dglx-direct=true -Dgbm=enabled \
	-Dgles1=disabled -Dgles2=enabled -Dgallium-va=disabled -Dteflon=false \
	-Dmesa-clc=system -Dprecomp-compiler=system
ninja -C "$B/build-arm64" src/gallium/targets/dri/libgallium-26.0.8-1ubuntu0.3.so
ls -la "$B/build-arm64/src/gallium/targets/dri/libgallium-26.0.8-1ubuntu0.3.so"
echo BUILD_DONE
