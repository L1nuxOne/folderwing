#!/usr/bin/env bash
# Runs only in the owned, pinned cross-build container. /work holds verified sources.
set -euo pipefail
S=/work/src
B=/work/build
P=/work/prefix
JOBS=${FOLDERWING_BUILD_JOBS:-4}
mkdir -p "$B" "$P" /work/logs /work/bin
export SOURCE_DATE_EPOCH=1792540800
export CFLAGS='-O2 -ffunction-sections -fdata-sections'
export CXXFLAGS="$CFLAGS"
export PKG_CONFIG_LIBDIR="$P/lib/pkgconfig:$P/share/pkgconfig"
export PKG_CONFIG_PATH="$PKG_CONFIG_LIBDIR"
export PATH="/work/bin:$PATH"
cat > /work/bin/fw-cc <<'WRAP'
#!/bin/sh
exec x86_64-w64-mingw32-gcc-posix "$@" -static-libgcc
WRAP
cat > /work/bin/fw-cxx <<'WRAP'
#!/bin/sh
exec x86_64-w64-mingw32-g++-posix "$@" -static-libgcc -static-libstdc++
WRAP
chmod +x /work/bin/fw-cc /work/bin/fw-cxx
rm -f /work/bin/dlltool
cat > /work/bin/dlltool <<'DLLTOOL'
#!/bin/sh
exec /usr/bin/x86_64-w64-mingw32-dlltool --as x86_64-w64-mingw32-as "$@"
DLLTOOL
chmod +x /work/bin/dlltool
cat > /work/cross.ini <<'CROSS'
[binaries]
c = '/work/bin/fw-cc'
cpp = '/work/bin/fw-cxx'
ar = 'x86_64-w64-mingw32-ar'
strip = 'x86_64-w64-mingw32-strip'
windres = 'x86_64-w64-mingw32-windres'
nasm = 'nasm'
pkg-config = 'pkg-config'
[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
[properties]
needs_exe_wrapper = true
[built-in options]
prefer_static = true
c_args = ['-O2', '-ffunction-sections', '-fdata-sections']
cpp_args = ['-O2', '-ffunction-sections', '-fdata-sections']
c_link_args = ['-static-libgcc', '-static-libstdc++', '-Wl,--gc-sections']
cpp_link_args = ['-static-libgcc', '-static-libstdc++', '-Wl,--gc-sections']
CROSS
cat > /work/cross.cmake <<'CMAKE'
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_C_COMPILER /work/bin/fw-cc)
set(CMAKE_CXX_COMPILER /work/bin/fw-cxx)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /work/prefix /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
CMAKE
M=(--cross-file=/work/cross.ini --prefix="$P" --libdir=lib --buildtype=release --default-library=static --wrap-mode=nodownload --auto-features=disabled -Db_ndebug=true)
C=(-GNinja -DCMAKE_TOOLCHAIN_FILE=/work/cross.cmake -DCMAKE_INSTALL_PREFIX="$P" -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF)
step(){
 local name=$1;shift
 local key
 key=$( { printf '%s\n' "$@"; cat /work/sources.json /work/cross.ini /work/cross.cmake; } | sha256sum | cut -d' ' -f1)
 if [[ -f "$B/$name.done" && $(cat "$B/$name.done") == "$key" ]];then echo "Cached $name";return;fi
 echo "Building $name"
 printf '%q ' "$@" > "/work/logs/$name.command";printf '\n' >> "/work/logs/$name.command"
 if ! "$@" > "/work/logs/$name.log" 2>&1;then tail -70 "/work/logs/$name.log";return 1;fi
 printf '%s' "$key" > "$B/$name.done"
}
cmake_build(){ local name=$1;shift;rm -rf "$B/$name";cmake -S "$S/$name" -B "$B/$name" "${C[@]}" "$@" && cmake --build "$B/$name" -j "$JOBS" && cmake --install "$B/$name";}
meson_build(){ local name=$1;shift;rm -rf "$B/$name";meson setup "$B/$name" "$S/$name" "${M[@]}" "$@" && meson compile -C "$B/$name" -j "$JOBS" && meson install -C "$B/$name";}
export -f cmake_build meson_build
# Functions above execute in this shell through step; command records retain exact options.
step zlib cmake_build zlib -DZLIB_BUILD_EXAMPLES=OFF
# This zlib version names its static archive libzs.a; every -lz must use it.
ln -sfn libzs.a "$P/lib/libz.a"
rm -f "$P/lib/libz.dll.a"
step freetype cmake_build freetype -DFT_DISABLE_ZLIB=TRUE -DFT_DISABLE_BZIP2=TRUE -DFT_DISABLE_PNG=TRUE -DFT_DISABLE_HARFBUZZ=TRUE -DFT_DISABLE_BROTLI=TRUE
step fribidi meson_build fribidi -Ddocs=false -Dbin=false -Dtests=false
step harfbuzz meson_build harfbuzz -Dtests=disabled -Dutilities=disabled -Dsubset=disabled -Draster=disabled -Dvector=disabled -Dgpu=disabled
step libass meson_build libass -Ddirectwrite=enabled -Dasm=enabled -Drequire-system-font-provider=true
step opus cmake_build opus -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_INSTALL_PKG_CONFIG_MODULE=ON
step dav1d meson_build dav1d -Denable_tools=false -Denable_tests=false -Denable_examples=false
# Shaderc's own DEPS pins these source revisions. No auto-fetch or tests/examples.
for n in glslang spirv-tools spirv-headers;do dest="$S/shaderc/third_party/$n";if [[ -d "$dest" && ! -L "$dest" ]];then rmdir "$dest";fi;ln -sfn "$S/$n" "$dest";done
step shaderc cmake_build shaderc -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_EXECUTABLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON -DSHADERC_ENABLE_WERROR_COMPILE=OFF -DENABLE_GLSLANG_BINARIES=OFF
# The static combined archive contains shaderc and its pinned compiler dependencies.
cat > "$P/lib/pkgconfig/shaderc.pc" <<PC
prefix=$P
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: shaderc
Description: Folderwing pinned static shaderc_combined
Version: 2026.4
Libs: -L\${libdir} -lshaderc_combined -lstdc++ -lwinpthread
Cflags: -I\${includedir}
PC
step spirv-cross cmake_build spirv-cross -DSPIRV_CROSS_SHARED=OFF -DSPIRV_CROSS_STATIC=ON -DSPIRV_CROSS_CLI=OFF -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_ENABLE_MSL=OFF -DSPIRV_CROSS_ENABLE_CPP=OFF -DSPIRV_CROSS_ENABLE_REFLECT=OFF -DSPIRV_CROSS_ENABLE_UTIL=OFF
cat > "$P/lib/pkgconfig/spirv-cross-c-shared.pc" <<PC
prefix=$P
libdir=\${prefix}/lib
includedir=\${prefix}/include/spirv_cross
Name: spirv-cross-c-shared
Description: Folderwing pinned static SPIRV-Cross C/HLSL backend
Version: 0.68.0
Libs: -L\${libdir} -lspirv-cross-c -lspirv-cross-hlsl -lspirv-cross-glsl -lspirv-cross-core -lstdc++ -lwinpthread
Cflags: -I\${includedir}
PC
dest="$S/libplacebo/3rdparty/fast_float";if [[ -d "$dest" && ! -L "$dest" ]];then rmdir "$dest";fi;ln -sfn "$S/fast-float" "$dest"
dest="$S/libplacebo/3rdparty/Vulkan-Headers";if [[ -d "$dest" && ! -L "$dest" ]];then rmdir "$dest";fi;ln -sfn "$S/vulkan-headers" "$dest"
step libplacebo meson_build libplacebo -Ddemos=false -Dtests=false -Dbench=false -Dd3d11=disabled -Dopengl=disabled -Dvulkan=disabled -Dshaderc=disabled -Dglslang=disabled
ffmpeg_build(){
 rm -rf "$B/ffmpeg";mkdir -p "$B/ffmpeg";pushd "$B/ffmpeg"
 "$S/ffmpeg/configure" --prefix="$P" --target-os=mingw32 --arch=x86_64 --enable-cross-compile --cross-prefix=x86_64-w64-mingw32- --cc=/work/bin/fw-cc --cxx=/work/bin/fw-cxx --pkg-config=pkg-config --pkg-config-flags=--static --enable-static --disable-shared --disable-gpl --disable-nonfree --enable-version3 --disable-autodetect --disable-programs --disable-doc --disable-debug --disable-network --disable-devices --disable-encoders --disable-muxers --enable-zlib --enable-libdav1d --enable-libopus --enable-d3d11va --enable-dxva2 --extra-cflags="$CFLAGS" --extra-cxxflags="$CXXFLAGS" --extra-ldflags='-static-libgcc -static-libstdc++ -Wl,--gc-sections' || return
 make -j "$JOBS" && make install || return
 popd
}
step ffmpeg ffmpeg_build
# libmpv remains one replaceable DLL. Both static compiler runtimes and pthread
# are requested at the final link so no compiler-runtime DLL is required.
step mpv meson_build mpv --default-library=shared -Dgpl=false -Dcplayer=false -Dlibmpv=true -Dbuild-date=false -Dlua=disabled -Djavascript=disabled -Dgl=disabled -Dd3d11=enabled -Dd3d-hwaccel=enabled -Dd3d9-hwaccel=enabled -Dshaderc=enabled -Dspirv-cross=enabled -Dwasapi=enabled -Dwin32-threads=enabled -Dzlib=enabled "-Dc_link_args=['-static','-static-libgcc','-static-libstdc++','-Wl,--gc-sections']" "-Dcpp_link_args=['-static','-static-libgcc','-static-libstdc++','-Wl,--gc-sections']"
mkdir -p /work/output/include/mpv
cp "$B/mpv/libmpv-2.dll" /work/output/
cp "$S/mpv"/include/mpv/*.h /work/output/include/mpv/
cp /toolchain-record/packages.tsv /work/output/toolchain-packages.tsv
x86_64-w64-mingw32-strip --strip-unneeded /work/output/libmpv-2.dll
sha256sum /work/output/libmpv-2.dll
printf 'Minimal LGPL media build finished.\n'
