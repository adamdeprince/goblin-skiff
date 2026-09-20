#!/bin/bash
# Modified for Goblin Skiff on 2026-09-19.
set -euo pipefail
work=${SKIFF_RM2_WORKSPACE:?Set SKIFF_RM2_WORKSPACE to the prepared ARM workspace}
work=$(cd "$work" && pwd)
export ZIG_GLOBAL_CACHE_DIR="$work/.cache/zig-global"
sdk="$work/.cache/sdk/sysroot-5.7.119"
cd "$work"
cmake -S webp-source -B webp-build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$work/cmake/RemarkableZig.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$work/prefix" \
  -DBUILD_SHARED_LIBS=OFF -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF \
  -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF \
  -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF \
  -DWEBP_BUILD_EXTRAS=OFF
cmake --build webp-build --parallel 16
cmake --install webp-build
export CC="$work/scripts/utilities/zig-cc.sh" CXX="$work/scripts/utilities/zig-cxx.sh"
export AR="$work/scripts/zig-ar.sh" RANLIB="$work/scripts/zig-ranlib.sh"
export CFLAGS=-O2 CXXFLAGS=-O2
export PKG_CONFIG_LIBDIR="$sdk/usr/lib/pkgconfig" PKG_CONFIG_SYSROOT_DIR="$sdk" PKG_CONFIG_PATH=
cd "$work/djvu-source"
export CXXFLAGS='-O2 -std=gnu++14'
autoreconf -fi
./configure --build=x86_64-pc-linux-gnu --host=arm-linux-gnueabihf \
  --prefix="$work/prefix" --enable-static --disable-shared \
  --disable-xmltools --disable-desktopfiles
make -C libdjvu -j16
make -C libdjvu install
make -C tools -j16 cjb2
cd "$work/source"
export CXXFLAGS=-O2
./autogen.sh
export PROTOC="$work/protoc/bin/protoc"
export WEBP_CFLAGS="-I$work/prefix/include"
export WEBP_LIBS="$work/prefix/lib/libwebp.a $work/prefix/lib/libsharpyuv.a -lm"
export DJVU_CFLAGS="-I$work/prefix/include"
export DJVU_LIBS="$work/prefix/lib/libdjvulibre.a -ljpeg -lpthread"
export CJB2=/home/root/.local/share/inkline-utilities/goblin-skiff/current/libexec/cjb2
./configure --build=x86_64-pc-linux-gnu --host=arm-linux-gnueabihf \
  --prefix=/home/root/.local/share/inkline-utilities/goblin-skiff/current \
  --without-librsync --without-libraptorq --without-fips-crypto --without-utempter \
  --with-zstd --disable-syslog --disable-examples --disable-completion --disable-ufw
make -j16
printf 'PASS: Goblin Skiff ARMv7 cross-build\n'
