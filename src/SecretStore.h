#pragma once

#include <QByteArray>
#include <QString>

// 密码持久化。
//   Windows    —— DPAPI，密文只有当前登录用户能解
//   其他平台   —— AES-256-CBC，密钥由本机 machine-id + 登录用户 + 常量盐派生
//
// 非 Windows 平台不再退化成 base64：配置文件会被用户拷贝、备份、同步，
// base64 等于明文，任何拿到文件的人一行命令就还原出口令。
// 机器绑定密钥提供的是与 DPAPI 同级别的保证——配置被拷到另一台机器解不开。
namespace SecretStore {

QString protect(const QString &plain);
QString unprotect(const QString &stored);

// 存储是否真的加密（非 Windows 历史上返回 false，因为当时是 base64）。
bool    isEncrypted();

// 加密后端**此刻是否真的可用**。
//
// 与 isEncrypted() 的区别：isEncrypted() 说的是"设计上会不会加密"，
// 这一项说的是"这台机器上到底能不能加密"——
// 非 Windows 上程序是 dlopen 系统里的 libcrypto，三个主版本依次试探
// （libcrypto.so.3 / .1.1 / .1.0.0），一个都找不到时会**优雅降级**：
// 登录口令不保存，其余功能照常，不会崩溃。
//
// 为什么要单独暴露：交付包里的自检脚本以前只检查"包里的 lib/ 目录下有没有
// libcrypto"—— 而包里**本来就不带** OpenSSL（带固定 soname 的 libcrypto
// 会让系统直接起不来，那正是改成 dlopen 的原因）。
// 于是这个判据恒为假，客户每次都看到一句
// 「保存的登录口令不会被加密」—— 而实际上系统的 libcrypto 找得到、口令是加密的。
// 一句与事实相反的话比不说更糟。让**程序自己**报这个事实，
// 判据和实现就永远在同一条代码路径上。
bool    cryptoBackendAvailable();

// 界面提示里用的加密方式名称，避免在 Linux 上显示"DPAPI"这种假信息。
QString backendName();

// 从磁盘读回来的字节里认出「这就是密钥材料」。
//
// 返回空 QByteArray = 不是密钥材料（调用方应当重新生成一份新的）。
//
// 为什么必须单独抽成函数、而且**不能先 trim 再判长度**：
// 密钥材料是 32 个**随机二进制字节**（不是文本）。随机字节的首/尾
// 各有约 2.7% 的概率恰好是空白字节（QChar::isSpace 认的那几个：
// 0x09~0x0D、0x20、0xA0），任意一端命中时先 trim() 就会把长度削到 31
// ⇒ 被误判成「密钥损坏」⇒ 重新生成并覆盖 ⇒ ini 里已存的密文口令
// **当场解不开**（用户得重新输一遍密码）。约 5% 的机器每次启动都会抽中。
//
// 这个判定必须**按字节原样**做，所以它得有自己的、可被自检直接调用的入口。
QByteArray keyMaterialFromBytes(const QByteArray &raw);

} // namespace SecretStore
