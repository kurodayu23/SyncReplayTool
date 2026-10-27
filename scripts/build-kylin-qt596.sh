#!/bin/bash
# =============================================================================
# SyncReplayTool —— 麒麟 V10 (x86_64) 重建版构建脚本
#
# 容器   : kylin-v10-real (Ubuntu 16.04 base, glibc 2.23, gcc 5.4.0)
# 头文件 : Ubuntu bionic qtbase5-dev 5.9.5 (含 moc/uic/rcc/qmake)
# 链接的 : 交付包自带的 Qt 5.9.6 运行时 (libQt5Core/Gui/Widgets.so.5.9.6)
# 目的   : 产出的二进制与客户机上的随包运行时 ABI 完全一致, 且只需 glibc <= 2.23
#
# 挂载:
#   /toolchain = AP7000-Build-Directories\SyncReplayTool\qt596-toolchain
#                (debs/ = bionic Qt5.9.5 dev 包; runtime/ = 交付包解出的 5.9.6 运行时)
#   /src       = SyncReplayTool 源码 (只读, 绝不修改)
#   /work      = ai-workspace (源码工作副本)
#   /out       = AP7000-Build-Directories\SyncReplayTool\build-kylin (构建产物)
# =============================================================================
set -eo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

TOOLCHAIN=${TOOLCHAIN:-/toolchain}
SRC=${SRC:-/src}
WORK=${WORK:-/work}
OUT=${OUT:-/out}
RUNTIME="$TOOLCHAIN/runtime/srt-client/lib"
PREFIX=/tmp/qt59root
DEBROOT=/tmp/debroot
SYSINC=/usr/include/x86_64-linux-gnu/qt5
SYSLIB=/usr/lib/x86_64-linux-gnu

echo "##### 0. 环境"
uname -m; gcc --version | sed -n 1p; ldd --version | sed -n 1p
echo -n "容器自带 libglib: "; ls $SYSLIB/libglib-2.0.so.0 2>/dev/null || echo "缺失"

echo "##### 1. 暂存 Qt 5.9.5 头文件/工具 + 5.9.6 运行时 -> $PREFIX"
rm -rf "$PREFIX" "$DEBROOT"
mkdir -p "$PREFIX"/bin "$PREFIX"/include "$PREFIX"/lib/pkgconfig "$DEBROOT"
for d in "$TOOLCHAIN"/debs/*.deb; do dpkg -x "$d" "$DEBROOT"; done

cp -a "$DEBROOT/usr/lib/qt5/bin/qmake" "$PREFIX/bin/"
for t in moc uic rcc; do cp -a "$DEBROOT/usr/lib/qt5/bin/$t" "$PREFIX/bin/"; done
if [ -d "$DEBROOT/usr/lib/qt5/mkspecs" ]; then
  cp -a "$DEBROOT/usr/lib/qt5/mkspecs" "$PREFIX/mkspecs"
else
  cp -a "$DEBROOT/usr/lib/x86_64-linux-gnu/qt5/mkspecs" "$PREFIX/mkspecs"
fi
QTINC="$DEBROOT/usr/include/x86_64-linux-gnu/qt5"
for m in "$QTINC"/*; do cp -a "$m" "$PREFIX/include/"; done
for l in libQt5Core libQt5Gui libQt5Widgets; do
  cp -a "$RUNTIME/$l.so" "$RUNTIME/$l.so.5" "$RUNTIME/$l.so.5.9.6" "$PREFIX/lib/"
done
for m in Core Gui Widgets; do
cat > "$PREFIX/lib/pkgconfig/Qt5$m.pc" <<EOF
prefix=\${pcfiledir}/../..
Name: Qt5$m
Description: Qt 5 $m module
Version: 5.9.5
Requires:
Libs: -L\${prefix}/lib -lQt5$m
Cflags: -I\${prefix}/include -I\${prefix}/include/Qt5$m
EOF
done

export PATH="$PREFIX/bin:$PATH"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
export LD_LIBRARY_PATH="$PREFIX/lib:$RUNTIME:${LD_LIBRARY_PATH:-}"

echo "##### 2. 把容器的 Qt 路径指向本前缀 (头文件 5.9.5 / 库 5.9.6)"
# 2a. 头文件: 系统 5.6.1 目录改名, 再软链到 5.9.5
[ -d "$SYSINC" ] && mv "$SYSINC" "$SYSINC.sys-5.6.1" || true
ln -sfn "$PREFIX/include" "$SYSINC"
# 2b. 库: 系统 5.6.1 实体改名, 再软链到随包 5.9.6 (保证 -L 顺序不影响结果)
for l in Core Gui Widgets; do
  f="$SYSLIB/libQt5$l.so.5"
  if [ -e "$f" ] && [ ! -L "$f" ]; then mv "$f" "$f.sys561"; fi
  ln -sfn "$PREFIX/lib/libQt5$l.so.5.9.6" "$f"
done
# 2c. 系统 pkg-config 文件挪走, 避免 5.6.1 抢答
mkdir -p /root/sys-pc-backup
for f in $SYSLIB/pkgconfig/Qt5*.pc; do
  [ -e "$f" ] && mv "$f" /root/sys-pc-backup/ 2>/dev/null || true
done
# 2d. qmake 里写死的 QT_HOST_BINS / mkspecs 也指向 5.9.5
mkdir -p /usr/lib/qt5/bin
for t in moc uic rcc qmake; do ln -sfn "$PREFIX/bin/$t" "/usr/lib/qt5/bin/$t"; done
if [ -d "$SYSLIB/qt5/mkspecs" ]; then mv "$SYSLIB/qt5/mkspecs" "$SYSLIB/qt5/mkspecs.sys-5.6.1"; fi
mkdir -p "$SYSLIB/qt5"
ln -sfn "$PREFIX/mkspecs" "$SYSLIB/qt5/mkspecs"
echo "  头文件 -> $(readlink -f $SYSINC)"
echo "  库     -> $(readlink -f $SYSLIB/libQt5Core.so.5)"

echo "##### 3. 工具自检"
"$PREFIX/bin/qmake" -v
"$PREFIX/bin/moc" --version
pkg-config --modversion Qt5Core
echo -n "moc 最高 GLIBC: "; objdump -T "$PREFIX/bin/moc" | grep -o 'GLIBC_[0-9.]*' | sort -V | tail -1

echo "##### 4. 复制源码到工作副本 (绝不改 /src)"
rm -rf "$WORK/src"
cp -r "$SRC/src" "$WORK/src"
cp "$SRC/SyncReplayTool.pro" "$WORK/SyncReplayTool.pro"
chmod -R u+w "$WORK/src" "$WORK/SyncReplayTool.pro"
echo "  工作副本文件数: $(find "$WORK/src" -type f | wc -l)"

echo "##### 5. qmake + make"
mkdir -p "$OUT"
cd "$OUT"
rm -f Makefile Makefile.Release Makefile.Debug .qmake.stash
"$PREFIX/bin/qmake" \
    "INCLUDEPATH+=$PREFIX/include $PREFIX/include/QtCore $PREFIX/include/QtGui $PREFIX/include/QtWidgets" \
    "LIBS+=-L$PREFIX/lib -Wl,-rpath-link,$RUNTIME" \
    "$WORK/SyncReplayTool.pro"

echo "  --- INCPATH ---"; grep -m1 '^INCPATH' Makefile | tr ' ' '\n' | grep -E 'qt5|QtCore|qt59' || true
echo "  --- LIBS ---";   grep -m1 '^LIBS' Makefile
# 断言: 头文件路径必须解析到我们的 5.9.5 前缀
case "$(readlink -f $SYSINC)" in
  "$PREFIX"*) echo "  [OK] 头文件来源正确";;
  *) echo "!!! 致命: 头文件来源不对: $(readlink -f $SYSINC)"; exit 1;;
esac

make -j"$(nproc)" 2>&1 | tail -30
mkdir -p "$OUT/release"
cp -f "$OUT/SyncReplayTool" "$OUT/release/SyncReplayTool"
BIN="$OUT/release/SyncReplayTool"
echo "##### 6. 产物"
file "$BIN"
ls -l "$BIN"
