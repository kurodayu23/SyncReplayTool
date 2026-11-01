#!/bin/sh
# 可选：给**没有桌面环境**的机器装"开机自启"（user 级 systemd unit）。
#
# 什么时候用这个脚本：
#   · 有桌面（麒麟/UKUI 等）→ 在软件「设置」里勾「开机自动启动」就够了。
#     程序会自己写 ~/.config/autostart/syncreplaytool.desktop（XDG autostart 规范），
#     桌面登录时自动拉起。**这一条才是常规路径，不用执行本脚本。**
#   · 纯服务器 / 只有 SSH 会话 / 没装桌面 → 桌面那套没人读，
#     这时用本脚本写一个 user 级 systemd unit。
#
# 为什么是 user 级：
#   · **不需要 root**（客户是运维人员，sudo 要密码，且不该为装个工具要 root）；
#   · **不碰系统级** /etc/systemd/system —— 那是机器管理员的东西，
#     我们的程序不该往里塞东西；
#   · 单元文件跟着用户走：~/.config/systemd/user/syncreplaytool.service。
#
# 【重要】本工具是**图形界面**程序。没有桌面时 run.sh 会自动落到离屏模式
# （日志输出到终端），所以服务能起来、但看不到窗口。
# 这种机器上它的用处是"到点自动同步"，不是"给人看界面"。
#
# 用法：
#   sh 安装自启.sh              装上（开机自启）
#   sh 安装自启.sh --uninstall  取消（删掉单元并 disable）
#   sh 安装自启.sh --status     只看状态，不改任何东西

HERE=$(cd "$(dirname "$0")" && pwd)
RUN="$HERE/run.sh"

UNIT_DIR="$HOME/.config/systemd/user"
UNIT="$UNIT_DIR/syncreplaytool.service"

MODE="install"
case "$1" in
    --uninstall) MODE="uninstall" ;;
    --status)    MODE="status" ;;
esac

# ---- 先确认包是完整的 ----
if [ ! -f "$RUN" ]; then
    echo "包里找不到 $RUN"
    echo "请重新完整解压一次压缩包（不要只拷一部分）。"
    exit 1
fi

if [ "$MODE" = "status" ]; then
    echo "单元文件：$UNIT"
    if [ -f "$UNIT" ]; then
        echo "--- 单元内容 ---"
        cat "$UNIT"
    else
        echo "（单元文件不存在 = 还没装）"
    fi
    if command -v systemctl >/dev/null 2>&1; then
        systemctl --user is-enabled syncreplaytool.service 2>&1 | sed 's/^/自启状态：/'
    fi
    exit 0
fi

if [ "$MODE" = "uninstall" ]; then
    if command -v systemctl >/dev/null 2>&1; then
        systemctl --user disable --now syncreplaytool.service 2>&1 | sed 's/^/  /'
    fi
    if [ -f "$UNIT" ]; then
        rm -f "$UNIT" && echo "已删除：$UNIT"
    else
        echo "本来就没有单元文件：$UNIT"
    fi
    if command -v systemctl >/dev/null 2>&1; then
        systemctl --user daemon-reload 2>&1 | sed 's/^/  /'
    fi
    echo
    echo "如果这台机器有桌面，还要到软件「设置」里取消勾选「开机自动启动」，"
    echo "否则桌面那套 ~/.config/autostart/syncreplaytool.desktop 仍然会拉起程序。"
    exit 0
fi

# ---- 写单元文件 ----
#
# 目录可能还不存在（刚装完系统的机器），所以**先建再建**。
mkdir -p "$UNIT_DIR" 2>/dev/null
if [ ! -d "$UNIT_DIR" ]; then
    echo "创建目录失败：$UNIT_DIR（也没有权限创建）"
    exit 1
fi

# ExecStart 用**绝对路径**。这里不能留 %k 之类的占位：
# systemd 不认识 desktop 文件那一套占位符，留着等于 ExecStart 指向一个不存在的路径，
# 服务会起不来，而 systemctl 只报一句看不懂的 "Invalid argument"。
cat > "$UNIT" <<EOF
[Unit]
Description=数据同步工具（开机自启）

[Service]
Type=simple
ExecStart=sh "$RUN"
Restart=on-failure
RestartSec=10

[Install]
WantedBy=default.target
EOF

if [ ! -f "$UNIT" ]; then
    echo "写单元文件失败：$UNIT"
    exit 1
fi
chmod 644 "$UNIT" 2>/dev/null

echo "单元文件已写入：$UNIT"
echo "启动命令：sh \"$RUN\""
echo

# ---- 让 systemd 认它 ----
if ! command -v systemctl >/dev/null 2>&1; then
    echo "⚠ 这台机器没有 systemctl，单元文件已经写好了，但没法自动 enable。"
    echo "  机器管理员可以手动执行："
    echo "    systemctl --user daemon-reload"
    echo "    systemctl --user enable syncreplaytool.service"
    exit 0
fi

systemctl --user daemon-reload 2>&1 | sed 's/^/  /'
systemctl --user enable syncreplaytool.service 2>&1 | sed 's/^/  /'

# enable 之后**读回来核对一次**。
# systemctl 的输出可能被 systemd 的日志级别吞掉，
# 于是"装上了"这句话可能没有依据 —— 与其相信输出，不如查状态。
if systemctl --user is-enabled syncreplaytool.service 2>/dev/null | grep -q '^enabled'; then
    echo
    echo "✓ 已启用开机自启（user 级，不需要 root）"
else
    echo
    echo "⚠ 没有确认启用成功。请手动执行下面两条并把输出发给技术支持："
    echo "    systemctl --user daemon-reload"
    echo "    systemctl --user enable syncreplaytool.service"
    echo "    systemctl --user status syncreplaytool.service"
fi

echo
echo "想立刻试一次：systemctl --user start syncreplaytool.service"
echo "想看日志：  journalctl --user -u syncreplaytool.service -f"
echo "想取消：    sh \"$HERE/安装自启.sh\" --uninstall"
echo
echo "提醒：桌面环境**不需要**执行本脚本 —— 在软件「设置」里勾「开机自动启动」即可。"
