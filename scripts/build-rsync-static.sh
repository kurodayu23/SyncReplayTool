#!/bin/bash
# 构建交付包自带的静态 rsync（离线交付核心依赖）。
#
# 为什么需要这个脚本
# -----------------
# 客户要求软件能完全离线运行。rsync over ssh 要求**两端**都有 rsync，而接收端
# 是被 sshd 拉起来的，按 login shell 的 PATH 找，看不到包目录。所以包里必须
# 自带一份能跑的 rsync，并且允许装到 /usr/local/bin（安装.sh --with-rsync）。
#
# 为什么必须静态链接
# -----------------
# 麒麟 V10 的 glibc 是 2.23。Ubuntu 22.04 构建的 rsync 要 GLIBC_2.38，在麒麟上
# 连启动都做不到（"GLIBC_2.38 not found"），这也是历史版本里 libexec/ 被整个删掉
# 的原因。静态链接后 ldd 报 "not a dynamic executable"，不依赖目标机任何 .so，
# 麒麟 V10 / V11 通用。
#
# 为什么要在 kylin-v10-real 容器里编
# ---------------------------------
# 容器里就是 glibc 2.23 + gcc，编出来的产物与目标机同代；在 Ubuntu 22.04/24.04
# 上编会引入新符号。脚本用 docker 跑，Windows / Linux 宿主都能用。
#
# 用法
# ----
#   scripts/build-rsync-static.sh                 # 默认输出到 work/rsync-static/rsync-static
#   scripts/build-rsync-static.sh <输出目录>       # 自定义输出目录
#
# 产物会被 scripts/package-delivery.ps1 的 -StaticRsync 参数使用；不传则包里没有
# 自带 rsync，客户机必须自己装 rsync 才能同步（离线环境会卡住）。

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
PROJECT=$(cd "$HERE/.." && pwd)
OUT_DIR=${1:-"$PROJECT/../AP7000-Build-Directories/SyncReplayTool/work/rsync-static"}
IMAGE=${SRT_RSYNC_BUILD_IMAGE:-kylin-v10-real:latest}
RSYNC_VER=${SRT_RSYNC_VERSION:-3.1.3}
SRC_URL="https://download.samba.org/pub/rsync/src/rsync-${RSYNC_VER}.tar.gz"

# 默认输出目录是 Windows 风格路径时转成容器能挂的绝对路径
mkdir -p "$OUT_DIR"
OUT_ABS=$(cd "$OUT_DIR" && pwd)

echo "== 构建静态 rsync $RSYNC_VER =="
echo "   镜像  : $IMAGE"
echo "   输出  : $OUT_ABS/rsync-static"
echo

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "!!! 镜像不存在：$IMAGE" >&2
    echo "    （这是麒麟 V10 实测用的编译环境，没有它编不出 glibc 2.23 的产物）" >&2
    exit 1
fi

docker run --rm \
    -v "${OUT_ABS}:/out" \
    --entrypoint /bin/bash \
    "$IMAGE" -c "
set -e
cd /tmp
echo '-- 下载源码 --'
rm -rf rsync.tar.gz rsync-${RSYNC_VER}
wget -q -O rsync.tar.gz '${SRC_URL}' || {
    echo '!!! 下载失败。这个构建步骤需要能访问 download.samba.org。' >&2
    echo '    离线机器请先把 rsync-${RSYNC_VER}.tar.gz 放到镜像里，或改用本地源。' >&2
    exit 1
}
tar xzf rsync.tar.gz
cd rsync-${RSYNC_VER}
echo '-- configure --'
./configure --prefix=/tmp/rs > /tmp/conf.log 2>&1 || { tail -20 /tmp/conf.log; exit 1; }
echo '-- make（静态链接）--'
make -j\$(nproc) LDFLAGS='-static' > /tmp/make.log 2>&1 || { tail -40 /tmp/make.log; exit 1; }
[ -f rsync ] || { echo '!!! 没有产出 rsync' >&2; exit 1; }
cp rsync /out/rsync-static
chmod 755 /out/rsync-static
echo
echo '-- 产物自检 --'
file /out/rsync-static
ldd /out/rsync-static 2>&1 | head -2
/out/rsync-static --version | head -1
"

echo
echo "== 校验 =="

# 三条硬判据：必须静态、必须能跑、必须不是动态依赖
FAILED=0
if ! file "$OUT_ABS/rsync-static" | grep -q "statically linked"; then
    echo "!!! 不是静态链接（file 没说 statically linked）" >&2
    FAILED=1
else
    echo "  ✓ 静态链接"
fi

if ldd "$OUT_ABS/rsync-static" 2>&1 | grep -q "=>"; then
    echo "!!! ldd 显示出动态依赖，换机器可能跑不了" >&2
    FAILED=1
else
    echo "  ✓ ldd 无动态依赖"
fi

if "$OUT_ABS/rsync-static" --version 2>/dev/null | head -1 | grep -q "rsync"; then
    echo "  ✓ 可执行，版本：$("$OUT_ABS/rsync-static" --version | head -1)"
else
    echo "!!! 跑不起来（可能是宿主架构不符，这个脚本只在 x86_64 有意义）" >&2
    FAILED=1
fi

SIZE=$(stat -c%s "$OUT_ABS/rsync-static" 2>/dev/null || stat -f%z "$OUT_ABS/rsync-static")
echo "  大小：$SIZE 字节"

if [ "$FAILED" != "0" ]; then
    echo
    echo "!! 产物不合格，不要用它打包" >&2
    exit 1
fi

echo
echo "== 完成 =="
echo "   $OUT_ABS/rsync-static"
echo
echo "   打包时传给："
echo "     scripts/package-delivery.ps1 -StaticRsync '$OUT_ABS/rsync-static' ..."