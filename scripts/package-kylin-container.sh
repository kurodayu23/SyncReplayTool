#!/bin/bash
# =============================================================================
# 麒麟 (x86_64) 交付包组装 —— **必须在 Linux 容器里跑**。
#
# 为什么不能在 Windows 上打：
#   1) lib/ 与 plugins/ 里有 27 个符号链接（libQt5Core.so.5 -> ...so.5.9.6）。
#      Windows 侧拷贝会把链接展开成实体文件，动态链接器随后去找
#      "libQt5Core.so.5" 这个 soname，而包里已经没有那个文件了。
#   2) Windows 上打的 tar 会毁掉权限位（自检.sh 必须带 x 位）。
#   3) 客户机是 Linux，打包产物必须用 Linux 的 tar 生成。
#
# 由 scripts/package-delivery.ps1 调用，不要单独跑。
#
# 用法（在容器里）：
#   bash /pkg.sh --binary /build/SyncReplayTool \
#                --base-tar /stage/base.tar.gz \
#                --payload /payload \
#                --out "/out/数据同步工具-麒麟.tar.gz" \
#                --root-name "数据同步工具-麒麟"
#
# 参数：
#   --binary    <file>  要打进包的 SyncReplayTool（Linux ELF，必需）
#   --base-tar  <file>  基础包：自带 Qt 运行时与插件的交付包
#   --payload   <dir>   运行时脚本目录（run.sh / 自检.sh / 安装.sh / 安装自启.sh /
#                       数据同步工具.desktop / qt.conf / bin/*）
#   --out       <file>  输出的 .tar.gz（必需）
#   --root-name <name>  包内根目录名（默认 数据同步工具-麒麟）
#   --work      <dir>   临时工作目录（默认 /work/pkg）
#
# ⚠ 2026-10-25（客户要求「包里不要文档」）：**交付包内不再带任何文档**。
#   原先这里会把《使用步骤与注意事项.md》装进包，现已整体移除；
#   手册原件只留在源码树内部留用，不进包、也不进 base tar。
#   下面的「文档闸门」在**装配阶段**直接拦住任何 .md/.txt/.pdf/.doc* 混进包里。
#
# 机器可读标记（宿主侧脚本靠它取数，不要改格式）：
#   SRT_BIN_SHA256=<64 hex>   包内二进制 sha256
#   SRT_ENTRIES=<n>           包内条目数
#   SRT_SYMLINKS=<n>          包内符号链接数
#   SRT_SELFCHK_MODE=<mode>   自检.sh 的权限位
#
# 关键取舍：payload 文件与二进制一律用 `cp -p`（保留 mtime）。
#   宿主侧的"时间戳闸门"要比对"包内 mtime vs 源文件 mtime"；
#   如果这里用普通 cp（mtime=现在），闸门就永远绿灯，**查不出旧包**。
# =============================================================================
set -eo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export LC_ALL=C.UTF-8

BINARY=""
BASE_TAR=""
PAYLOAD=""
DOCS=""
OUT=""
ROOT_NAME="数据同步工具-麒麟"
ROOT_NAME_B64=""
WORK="/work/pkg"

while [ $# -gt 0 ]; do
    case "$1" in
        --binary)    BINARY=$2;  shift 2 ;;
        --base-tar)  BASE_TAR=$2; shift 2 ;;
        --payload)   PAYLOAD=$2;  shift 2 ;;
        --docs)      DOCS="$2"; shift 2 ;;  # 已废弃：交付包不再带文档（2026-10-25）
        --out)       OUT=$2;      shift 2 ;;
        --root-name) ROOT_NAME=$2; shift 2 ;;
        --root-name-b64) ROOT_NAME_B64=$2; shift 2 ;;
        --work)      WORK=$2;     shift 2 ;;
        -h|--help)   sed -n '2,45p' "$0"; exit 0 ;;
        *) echo "未知参数：$1" >&2; exit 2 ;;
    esac
done

# 根目录名走 base64：宿主侧的 docker 命令行是纯 ASCII 的，
# 中文根目录名如果直接进命令行，不同 shell / 代码页下会被拆坏。
if [ -n "$ROOT_NAME_B64" ]; then
    ROOT_NAME=$(printf '%s' "$ROOT_NAME_B64" | base64 -d)
fi

fail() { echo "!!! $*" >&2; exit 1; }

[ -n "$BINARY" ] || fail "缺少 --binary"
[ -n "$OUT" ]    || fail "缺少 --out"
[ -f "$BINARY" ] || fail "找不到二进制：$BINARY"
[ -n "$BASE_TAR" ] || fail "缺少 --base-tar"
[ -f "$BASE_TAR" ] || fail "找不到基础包：$BASE_TAR"
[ -n "$PAYLOAD" ] && [ -d "$PAYLOAD" ] || fail "找不到运行时脚本目录：$PAYLOAD"

# 运维文档：**已整体移除**（2026-10-25 客户要求「包里不要文档」）。
#   --docs 参数只为兼容旧调用方保留：传了就打一行提示然后忽略，
#   绝不能再把 .md 写进包里。
if [ -n "$DOCS" ]; then
    echo "  [注意] --docs 已废弃：交付包不再包含文档，该参数已忽略"
fi

echo "##### 0. 参数"
echo "  binary    : $BINARY"
echo "  base tar  : $BASE_TAR"
echo "  payload   : $PAYLOAD"
echo "  docs      : 不再随包交付（2026-10-25 起）"
echo "  out       : $OUT"
echo "  root name : $ROOT_NAME"

rm -rf "$WORK"
mkdir -p "$WORK/stage" "$WORK/out"
STAGE="$WORK/stage"

# ---------------------------------------------------------------- 1. 基础包
echo "##### 1. 解开基础包"
tar -xzf "$BASE_TAR" -C "$STAGE"
OLD_ROOT=$(find "$STAGE" -mindepth 1 -maxdepth 1 -type d | head -1)
[ -n "$OLD_ROOT" ] || fail "基础包里找不到根目录"
if [ "$(basename "$OLD_ROOT")" != "$ROOT_NAME" ]; then
    mv "$OLD_ROOT" "$STAGE/$ROOT_NAME"
fi
PKG="$STAGE/$ROOT_NAME"
echo "  根目录 -> $ROOT_NAME"
echo "  解出条目：$(find "$PKG" | wc -l)"

# ---------------------------------------------------------------- 2. 裁剪
echo "##### 2. 裁掉死重"
# 这批需要 GLIBC_2.38，麒麟 V10（glibc 2.23）永不可用。
rm -rf "$PKG/libexec" "$PKG/lib/sshv2"
# 用不上的插件
rm -rf "$PKG/plugins/imageformats" "$PKG/plugins/egldeviceintegrations"
rm -f  "$PKG/plugins/platforms/libqlinuxfb.so" \
       "$PKG/plugins/platforms/libqvnc.so" \
       "$PKG/plugins/platforms/libqeglfs.so" \
       "$PKG/plugins/platformthemes/libqgtk3.so"
find "$PKG/plugins" -mindepth 1 -type d -empty -delete 2>/dev/null || true

# ---- 裁掉 Qt 死重库（麒麟 V10 实机测出来的）----
#
# 【P0 教训 · 2026-10-23】"程序不用" != "包里不用"。
# 上一轮按"程序不用 Qt DBus / Svg / Network"把三个库裁掉了，自检 43 PASS 全绿就放行。
# 结果麒麟 V10 真机上**图形界面永远起不来**：
#   plugins/platforms/libqxcb.so: libQt5Core.so.5: version `Qt_5.15' not found
# 原因：libqxcb.so 与 libQt5XcbQpa.so 的 DT_NEEDED 里**有** libQt5DBus.so.5。
# 所以依赖关系必须用 readelf -d 实测，不能凭"程序用不用"判断。
#
# 顺序陷阱：必须**先删符号链接、再搬实体**。
# 原来先 mv 实体，链接的 readlink -f 目标当场就没了，比对永远不成立 ——
# 结果包里留下 6 个悬空符号链接。
for dead in libQt5EglFSDeviceIntegration.so.5.9.6 \
            libQt5EglFsKmsSupport.so.5.9.6 ; do
    [ -f "$PKG/lib/$dead" ] || continue
    for l in "$PKG"/lib/*; do
        [ -L "$l" ] || continue
        _srt_t=$(readlink "$l" 2>/dev/null)
        case "$_srt_t" in
            "$dead") rm -f "$l" ;;
        esac
    done
    mv "$PKG/lib/$dead" "$WORK/dead-$dead"
done

# ---- 闸门 A：包里不许有悬空符号链接 ----
# 悬空链接 = 实体被裁掉但链接还在。loader 遇到它报的是"文件找不到"，
# 客户看到的是"软件坏了"，而真正原因是打包脚本裁漏了。
_dangling=$(find "$PKG" -xtype l 2>/dev/null | head -20)
if [ -n "$_dangling" ]; then
    echo "  [闸门] dangling symlinks in package:"
    echo "$_dangling" | sed 's/^/        /'
    fail "包内有悬空符号链接（实体被裁掉但链接还在），交付前必须修掉"
fi
echo "  悬空链接闸门   : 通过（0 个）"

# ---- 闸门 B：插件依赖必须全部可解析 ----
# 裁掉 libQt5DBus 会让界面在真机上起不来，但离屏自检照样全绿。
# 这里直接拦下来，不再靠自检。
_gate_ok=1
for so in "$PKG"/plugins/*/*.so "$PKG"/plugins/*.so; do
    [ -e "$so" ] || continue
    _g_real=$(readlink -f "$so" 2>/dev/null || echo "$so")
    _g_ldd=$(LD_LIBRARY_PATH="$PKG/lib" ldd "$_g_real" 2>&1)
    if echo "$_g_ldd" | grep -q 'not found'; then
        echo "  [闸门] 插件依赖无法解析: ${_g_real#$PKG/}"
        echo "$_g_ldd" | grep 'not found' | sed 's/^/        /'
        _gate_ok=0
    fi
    for _g_dep in $(echo "$_g_ldd" | awk '$1 ~ /^libQt5/ {print $1}' | sort -u); do
        if [ ! -e "$PKG/lib/$_g_dep" ]; then
            echo "  [闸门] 插件需要的 $_g_dep 不在包内（会回落到系统 Qt，版本对不上）"
            _gate_ok=0
        fi
    done
done
if [ "$_gate_ok" -ne 1 ]; then
    fail "裁剪后插件依赖有无法解析的，交付包会在客户机上起不来界面（日志见上）"
fi
echo "  插件依赖闸门   : 通过（全部可解析）"
echo "  libexec/        : 已删除（GLIBC_2.38，麒麟 V10 不可用）"
echo "  lib/sshv2/      : 已删除（同上）"
echo "  imageformats/   : 已删除（只同步 .spe/.iq，用不到）"
echo "  egldevice + linuxfb/vnc/eglfs + libqgtk3 : 已删除"
echo "  Qt 死重库       : 已删除 EglFS×2（无关）；DBus/Network 必须保留（插件依赖）"

# ---------------------------------------------------------------- 3. 运行时脚本
echo "##### 3. 覆盖运行时脚本"
# 自检.sh 与 安装.sh 都会让用户执行 包里的 安装自启.sh（纯服务器场景）。
# 上一版包里**漏了**这个文件 -> 用户照着提示执行必然 "No such file"。
# 这里补上，并把"清单"显式化：少一个文件就 fail。
PAYLOAD_FILES="run.sh 自检.sh 安装.sh 安装自启.sh 数据同步工具.desktop qt.conf"
for f in $PAYLOAD_FILES; do
    [ -f "$PAYLOAD/$f" ] || fail "payload 里缺 $f"
    cp -p "$PAYLOAD/$f" "$PKG/$f"
done
mkdir -p "$PKG/bin"
for f in rsync ssh scp ssh-keygen ssh-keyscan; do
    [ -f "$PAYLOAD/bin/$f" ] || fail "payload 里缺 bin/$f"
    cp -p "$PAYLOAD/bin/$f" "$PKG/bin/$f"
done

# ---- 闸门 C：包里被文档/脚本引用的文件必须真的在包里 ----
# 这一条把"上一版漏装 安装自启.sh"这类问题挡在打包阶段，
# 而不是等客户在服务器上执行命令才发现。
for f in $PAYLOAD_FILES; do
    [ -e "$PKG/$f" ] || fail "装配后包里仍然缺 $f"
done
echo "  运行时脚本装配 : 通过（$(echo $PAYLOAD_FILES | wc -w) 个文件 + bin/5 个）"

# ---------------------------------------------------------------- 4. 二进制
echo "##### 4. 装入新二进制"
cp -p "$BINARY" "$PKG/SyncReplayTool"
chmod 755 "$PKG/SyncReplayTool"
file "$PKG/SyncReplayTool" 2>/dev/null || true
echo "  sha256: $(sha256sum "$PKG/SyncReplayTool" | cut -d' ' -f1)"

# ---------------------------------------------------------------- 5. 文档闸门
echo "##### 5. 文档闸门（包内不得有任何文档）"
# 2026-10-25 客户要求：交付包里只留"打开就能用的工具"，
# 任何 .md/.txt 之类的说明文档都不许出现（手册原件只留在源码树内部留用）。
# 这里在**装配阶段**就拦住 —— 等到客户机器上才发现，等于没拦。
_doc_all=$(find "$PKG" -type f \
    \( -iname '*.md' -o -iname '*.txt' -o -iname '*.pdf' -o -iname '*.doc' \
       -o -iname '*.docx' -o -iname '*.rtf' -o -iname '*.odt' \
       -o -iname '*.htm' -o -iname '*.html' \) 2>/dev/null)
if [ -n "$_doc_all" ]; then
    echo "  [闸门] 包内出现文档文件："
    printf '%s\n' "$_doc_all" | sed -n '1,20p' | sed "s|^$PKG/|        |"
    fail "交付包内不得包含文档（客户要求）"
fi
echo "  文档闸门       : 通过（0 个文档文件）"

# ---------------------------------------------------------------- 6. 换行/权限
echo "##### 6. 统一换行符与权限位"
# Windows 上编辑过的脚本带 CRLF，dash 会在每行末尾留一个 \r，
# 于是 "$HERE/自检.sh" 这种判断永远不成立（"no such file"）。
CRLF_FIXED=0
for f in "$PKG/run.sh" "$PKG/自检.sh" "$PKG/安装.sh" "$PKG/安装自启.sh" "$PKG"/bin/*; do
    if grep -q $'\r' "$f" 2>/dev/null; then
        sed -i 's/\r$//' "$f"
        CRLF_FIXED=$((CRLF_FIXED + 1))
    fi
done
echo "  CRLF 修正：$CRLF_FIXED 个文件"
chmod 755 "$PKG/run.sh" "$PKG/自检.sh" "$PKG/安装.sh" "$PKG/安装自启.sh" "$PKG"/bin/*
chmod 644 "$PKG/数据同步工具.desktop" "$PKG/qt.conf" 2>/dev/null || true

# ---------------------------------------------------------------- 7. 打包
echo "##### 7. 打包"
OUTDIR=$(dirname "$OUT")
mkdir -p "$OUTDIR"
cd "$STAGE"
# 不加 -h：符号链接必须原样保留（-h 是 --dereference，正好相反）
tar --owner=0 --group=0 --numeric-owner --sort=name \
    -czf "$OUT" "$ROOT_NAME"
ls -l "$OUT"
echo "  sha256: $(sha256sum "$OUT" | cut -d' ' -f1)"
echo "  条目数: $(tar -tzf "$OUT" | wc -l)"

# ---------------------------------------------------------------- 8. 自检
echo "##### 8. 解包复核"
rm -rf "$WORK/verify"
mkdir -p "$WORK/verify"
tar -xzf "$OUT" -C "$WORK/verify"
V="$WORK/verify/$ROOT_NAME"
_SYM=$(find "$V" -type l 2>/dev/null | wc -l)
echo "  解包后符号链接: $_SYM"
echo "  二进制 sha256 : $(sha256sum "$V/SyncReplayTool" | cut -d' ' -f1)"
# 机器可读标记：宿主侧用它在**不打散符号链接、不碰中文路径**的前提下，
# 把"包里的那一份二进制"和"编译产物"逐字节对账。
echo "SRT_BIN_SHA256=$(sha256sum "$V/SyncReplayTool" | cut -d' ' -f1)"
echo "SRT_ENTRIES=$(tar -tzf "$OUT" | wc -l)"
echo "SRT_SYMLINKS=$_SYM"
echo "SRT_SELFCHK_MODE=$(stat -c '%a' "$V/自检.sh" 2>/dev/null || echo unknown)"
for f in run.sh 自检.sh 安装.sh 安装自启.sh 数据同步工具.desktop qt.conf; do
    [ -e "$V/$f" ] && echo "  [有] $f" || echo "  [缺] $f"
done
for f in rsync ssh scp ssh-keygen ssh-keyscan; do
    [ -x "$V/bin/$f" ] && echo "  [有] bin/$f" || echo "  [缺/不可执行] bin/$f"
done
echo "  libexec/ 残留: $([ -e "$V/libexec" ] && echo 有 || echo 无)"
echo "  lib/sshv2/ 残留: $([ -e "$V/lib/sshv2" ] && echo 有 || echo 无)"
echo "  包内文档文件数: $(find "$V" -type f \( -iname '*.md' -o -iname '*.txt' -o -iname '*.pdf' \) 2>/dev/null | wc -l)（必须为 0）"
echo "##### 打包完成: $OUT"
