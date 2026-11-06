#!/bin/sh
# 创建桌面图标；可选地安装「开机自动启动」。
#
# 为什么需要这一步：包里那份 数据同步工具.desktop 写的是 Exec=sh "%k/run.sh"，
# %k 是"这个 desktop 文件自己所在的目录"。图标被拷到桌面之后 %k 就变成桌面目录，
# 那里没有 run.sh，双击会静默失败（Terminal=false，报错也没地方去）。
# 实测 2026-10-19 银河麒麟桌面：双击图标毫无反应，界面不出现、日志一片干净。
#
# 所以这里把包的**绝对路径**写进图标，图标从此指向真正的程序。
# 整个过程不装任何东西、不需要管理员权限。
#
# ----------------------------------------------------------------------------
# 开机自动启动（**可选，默认不装**）
#
#   sh 安装.sh --autostart      装上开机自启
#   sh 安装.sh --no-autostart   取消开机自启（删掉自启文件）
#
# 为什么默认不装：开机就自动拉起一个会连远端、起 rsync、建 SSH 的程序，
# 得由客户自己决定。**装之前请先在软件「设置」里配好远端并试跑一轮。**
#
# 为什么用 XDG autostart（~/.config/autostart/*.desktop）而不是 systemd：
#   · **不需要 root**，也不碰 /etc/systemd/system（那是机器管理员的东西）；
#   · 桌面环境（麒麟/UKUI）原生认这个规范；
#   · Exec 指向 sh "<包目录>/run.sh" —— 裸二进制缺 LD_LIBRARY_PATH /
#     QT_PLUGIN_PATH，Qt 找不到平台插件会直接 abort(134)。
# 纯服务器（没有桌面）请改用 包里的 安装自启.sh（写 user 级 systemd unit）。
#
# ----------------------------------------------------------------------------
# 自带 rsync 装到系统里（**离线交付用**）
#
#   sudo sh 安装.sh --with-rsync
#
# 为什么要这一步：rsync over ssh 要求**两端都有 rsync**。接收端是被 sshd 拉起来的，
# 它用 login shell 的 PATH（/usr/local/bin:/usr/bin:...）去找 rsync，**看不到包目录**。
# 客户机器如果没装 rsync、又不能上外网 apt install，光有包内那份是传不了的 ——
# 实测报错：rsync error: remote command not found (code 127) / bash: rsync: 未找到命令。
#
# 本包 libexec/rsync 是在麒麟 V10 里静态编译的（LDFLAGS=-static，ldd 无动态依赖），
# 装到 /usr/local/bin/rsync 后 sshd 也能找到，接收端就不必预装任何东西。
# 需要管理员权限；已经装过同一份会跳过。
# ----------------------------------------------------------------------------
# ----------------------------------------------------------------------------

HERE=$(cd "$(dirname "$0")" && pwd)
TPL="$HERE/数据同步工具.desktop"

# ---- 参数 ----
DO_AUTOSTART="ask"      # ask | yes | no
DO_RSYNC="no"          # no | yes（把 libexec/rsync 装到 /usr/local/bin）
while [ $# -gt 0 ]; do
    case "$1" in
        --autostart)    DO_AUTOSTART="yes" ;;
        --no-autostart) DO_AUTOSTART="no" ;;
        --with-rsync)   DO_RSYNC="yes" ;;
        # 2026-11-06 审查 P1：原来只认 $1（`case "${1:-}"`），
        # 于是 `sh 安装.sh --autostart --with-rsync` 会把 --with-rsync **静默丢掉**——
        # 自启装了、rsync 没装，同步照样报 remote command not found (127)。
        # 两个参数都是本脚本自己在头部推荐的，必须都能被认。
        *)
            echo "未知参数：$1" >&2
            echo "可用：--autostart | --no-autostart | --with-rsync" >&2
            exit 2 ;;
    esac
    shift
done

# ---- 自带 rsync 装到系统（--with-rsync）----
#
# 为什么装到 /usr/local/bin 而不是 /usr/bin：
#   · /usr/local/bin 是给"管理员本地安装的软件"用的，不覆盖发行版的文件；
#   · 它在 login shell 的默认 PATH 里（实测：/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:...），
#     所以 sshd 拉起来的 `rsync --server` 能直接找到；
#   · 系统以后自己升级 rsync 也不会把我们这份覆盖掉，反之亦然。
if [ "$DO_RSYNC" = "yes" ]; then
    SRC="$HERE/libexec/rsync"
    DST="/usr/local/bin/rsync"
    if [ ! -f "$SRC" ]; then
        echo "✗ 包里没有 libexec/rsync，无法安装。这个包可能是裁剪过的。" >&2
        exit 1
    fi
    if [ "$(id -u)" != "0" ]; then
        echo "✗ 需要管理员权限。请改用：sudo sh 安装.sh --with-rsync" >&2
        exit 1
    fi
    # 已经装过同一份就跳过（按大小+首行版本判断，避免重复覆盖）
    if [ -f "$DST" ] && [ "$(stat -c%s "$DST" 2>/dev/null)" = "$(stat -c%s "$SRC" 2>/dev/null)" ] \
       && "$DST" --version 2>/dev/null | head -1 | grep -q "3.1.3"; then
        echo "✓ /usr/local/bin/rsync 已经是包里那一份（3.1.3），跳过"
    else
        install -m 755 "$SRC" "$DST" || { echo "✗ 复制失败：$DST" >&2; exit 1; }
        echo "✓ 已把自带 rsync 装到 $DST"
    fi
    # 闸门：装完之后必须能在 login shell 的 PATH 里找到、且能报版本
    if bash -lc "command -v rsync" >/dev/null 2>&1; then
        echo "  login shell 能看到：$(bash -lc "command -v rsync") → $(bash -lc "rsync --version" 2>/dev/null | head -1)"
    else
        echo "⚠ 装好了但 login shell 的 PATH 里找不到 rsync（PATH=$(bash -lc "echo \$PATH" 2>/dev/null)）" >&2
        echo "  接收端仍会报 remote command not found。请把 $DST 所在目录加进 PATH。" >&2
    fi
    echo "  取消安装：sudo rm -f $DST"
    # 2026-11-06 审查 P2：装完就停。原来装完继续走下面的桌面图标分支，
    # sudo 下 HOME=/root，会顺手 mkdir /root/桌面 并写图标 —— 一个系统级动作
    # 污染 root 家目录；而且那一堆输出会淹没 rsync 的安装结果，运维看不见。
    exit 0
fi

# ---- 开机自启：取消 ----
AUTOSTART_DIR="$HOME/.config/autostart"
AUTOSTART_FILE="$AUTOSTART_DIR/syncreplaytool.desktop"

if [ "$DO_AUTOSTART" = "no" ]; then
    if [ -f "$AUTOSTART_FILE" ]; then
        rm -f "$AUTOSTART_FILE" && echo "已删除开机自启文件：$AUTOSTART_FILE"
    else
        echo "本来就没有开机自启文件：$AUTOSTART_FILE"
    fi
    exit 0
fi

# ---- 开机自启：安装 ----
if [ "$DO_AUTOSTART" = "yes" ]; then
    if [ ! -f "$HERE/run.sh" ]; then
        echo "包里找不到 $HERE/run.sh"
        echo "请重新完整解压一次压缩包（不要只拷一部分）。"
        exit 1
    fi
    mkdir -p "$AUTOSTART_DIR" 2>/dev/null
    if [ ! -d "$AUTOSTART_DIR" ]; then
        echo "创建目录失败：$AUTOSTART_DIR（也没有权限创建）"
        exit 1
    fi
    # Exec 必须指向 run.sh（绝对路径），不能是裸二进制。
    cat > "$AUTOSTART_FILE" <<EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=数据同步工具
Comment=把本地数据同步到另一台电脑的指定路径
Exec=sh "$HERE/run.sh"
Path=$HERE
Icon=system-file-share
Terminal=false
X-GNOME-Autostart-enabled=true
# 与包里的 .desktop 保持同一口径：主分类只有一个（Network），其余是附加分类。
Categories=Network;FileTransfer;
EOF
    if [ ! -f "$AUTOSTART_FILE" ]; then
        echo "写自启文件失败：$AUTOSTART_FILE"
        exit 1
    fi
    chmod 644 "$AUTOSTART_FILE" 2>/dev/null
    echo "开机自启已安装：$AUTOSTART_FILE"
    echo "开机时执行：sh \"$HERE/run.sh\""
    echo
    echo "提醒：这只负责**把程序打开**，不会替你打开自动同步。"
    echo "      要每天定时同步，到软件「设置」里选「每天定时」并保存。"
    echo "想取消：sh \"$HERE/安装.sh\" --no-autostart"
    exit 0
fi

# ----------------------------------------------------------------------------
# 默认：只创建桌面图标（不装开机自启）
# ----------------------------------------------------------------------------

if [ ! -f "$TPL" ]; then
    echo "包里找不到 $TPL"
    echo "请重新完整解压一次压缩包（不要只拷一部分）。"
    exit 1
fi

# 桌面目录：xdg-user-dir 优先，其次 $HOME/Desktop。
DESK=$(xdg-user-dir DESKTOP 2>/dev/null)
[ -n "$DESK" ] || DESK=$HOME/Desktop

# 桌面目录可能还不存在（刚装完系统的机器、或用 root 执行时），
# 所以**先建再建**。原来的写法把 mkdir 写在 exit 1 后面，那一行是死代码，
# 目录不存在时直接报错退出，"先建桌面目录"这件事从来没发生过。
mkdir -p "$DESK" 2>/dev/null

if [ ! -d "$DESK" ]; then
    echo "找不到桌面目录：$DESK（也没有权限创建）"
    echo "你仍然可以直接运行：sh \"$HERE/run.sh\""
    exit 1
fi

OUT="$DESK/数据同步工具.desktop"

# 只替换 Exec / Path 两行，其余（名称、分类、图标）原样保留。
sed -e "s|^Exec=.*|Exec=sh \"$HERE/run.sh\"|" \
    -e "s|^Path=.*|Path=$HERE|" \
    "$TPL" > "$OUT" || exit 1

chmod +x "$OUT"

# 麒麟/UKUI 桌面要求启动器带"受信任"标记才肯运行。
# gio set 失败时**静默无输出**，结果是图标双击毫无反应（不报错、不弹窗），
# 运维只会认为"软件坏了"。所以这里设完立刻读回来核对一次，
# 没设上就把手动步骤直接告诉他。
TRUSTED=0
if command -v gio >/dev/null 2>&1; then
    gio set "$OUT" metadata::trusted true >/dev/null 2>&1
    if gio info -a metadata::trusted "$OUT" 2>/dev/null | grep -qi 'true'; then
        TRUSTED=1
    fi
fi

echo "桌面图标已创建：$OUT"
echo "指向：sh \"$HERE/run.sh\""
echo
if [ "$TRUSTED" = 1 ]; then
    echo "现在可以双击桌面上的「数据同步工具」图标启动。"
else
    echo "⚠ 没能自动设置「允许运行」标记（这台机器/这个文件系统可能不支持）。"
    echo "  请在桌面的图标上**右键 → 允许运行**（或「属性 → 权限 → 允许执行」），"
    echo "  否则双击不会有任何反应，也不会报错。"
fi
echo
echo "提醒：**不要手工把包里的 .desktop 拷到桌面**，必须执行 sh 安装.sh。"
echo "      手工拷过去的话，图标里的路径会失效，双击同样毫无反应。"
echo
echo "不想用图标也可以随时敲这一条："
echo "  sh \"$HERE/run.sh\""
echo
echo "要开机自动启动（可选）："
echo "  sh \"$HERE/安装.sh\" --autostart"
echo "  取消：sh \"$HERE/安装.sh\" --no-autostart"
