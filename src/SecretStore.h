#pragma once

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

// 界面提示里用的加密方式名称，避免在 Linux 上显示"DPAPI"这种假信息。
QString backendName();

} // namespace SecretStore
