#include "SecretStore.h"

// 用 _WIN32 而不是 Q_OS_WIN：这个定制 Qt 5.9.6 + VS2015 组合下 Q_OS_WIN 并没有
// 被 qsystemdetection.h 定义，写 Q_OS_WIN 会让整个 DPAPI 分支变成死代码。
#ifdef _WIN32
#include <windows.h>
#include <dpapi.h>

bool SecretStore::isEncrypted() { return true; }

// Windows 用 DPAPI，系统自带，没有"找不到后端"这回事。
bool SecretStore::cryptoBackendAvailable() { return true; }

QString SecretStore::backendName()
{
    return QString::fromUtf8(u8"DPAPI");
}

QString SecretStore::protect(const QString &plain)
{
    if (plain.isEmpty())
        return QString();

    const QByteArray utf8 = plain.toUtf8();
    DATA_BLOB in;
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(utf8.constData()));
    in.cbData = static_cast<DWORD>(utf8.size());

    DATA_BLOB out;
    // CRYPTPROTECT_UI_FORBIDDEN：明确禁止 DPAPI 在任何情况下弹 UI。
    // 传 dwFlags=0 时是否弹窗不受我们控制，而这个工具全程不允许模态框出现在
    // 保存配置的调用栈里（saveConfig 由 closeEvent 触发）。
    const DWORD kFlags = CRYPTPROTECT_UI_FORBIDDEN;
    if (!CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr, kFlags, &out))
        return QString();

    const QByteArray raw(reinterpret_cast<char *>(out.pbData), static_cast<int>(out.cbData));
    LocalFree(out.pbData);
    return QString::fromLatin1(raw.toBase64());
}

QString SecretStore::unprotect(const QString &stored)
{
    if (stored.isEmpty())
        return QString();

    const QByteArray raw = QByteArray::fromBase64(stored.toLatin1());
    DATA_BLOB in;
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(raw.constData()));
    in.cbData = static_cast<DWORD>(raw.size());

    DATA_BLOB out;
    const DWORD kFlags = CRYPTPROTECT_UI_FORBIDDEN;
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, kFlags, &out))
        return QString();

    const QByteArray plain(reinterpret_cast<char *>(out.pbData), static_cast<int>(out.cbData));
    LocalFree(out.pbData);
    return QString::fromUtf8(plain);
}

#else

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSysInfo>
#include <QUuid>

#include "ConfigStore.h"

#include <pwd.h>
#include <unistd.h>

#include <dlfcn.h>

// ---------------------------------------------------------------------------
// 非 Windows：AES-256-CBC，密钥由本机 machine-id + 登录用户 + 常量盐派生。
//
// 为什么以前只做 base64是不行的：
//   配置文件会被用户拷贝、同步、备份。base64 等于明文，
//   任何拿到文件的人 `base64 -d` 一行就还原出口令。
//   DPAPI 在 Windows 上提供的保证是"换机器/换用户解不开"，这里用机器绑定密钥
//   补上同一级别的保证。
//
// 为什么用 OpenSSL 而不是自己写 AES：密码学实现必须经过审计。
//
// 为什么是**运行时加载**而不是链接期依赖 —— 这是踩过的坑：
// 原来这里是 `#include <openssl/evp.h>` + 链接 `-lcrypto`，产物 NEEDED 里
// 留下了一条 `libcrypto.so.1.0.0`。这句话当年写的是：
//   「本工具本来就调用 openssh 的 ssh/rsync，openssh 自身链接 libcrypto，
//     所以任何能跑 ssh 的机器一定已经有 libcrypto。」
// **这句推理是错的**：libcrypto 的文件名带**主版本 soname**，跨主版本
// 就是另一个文件，不是同一个东西的两个名字。
//   OpenSSL 1.0.x -> libcrypto.so.1.0.0
//   OpenSSL 1.1.x -> libcrypto.so.1.1
//   OpenSSL 3.x   -> libcrypto.so.3
// 麒麟 V10/V11 装的主版本并不相同，而「机器上有某个 libcrypto」推不出
// 「机器上有 libcrypto.so.1.0.0」。后果是干净机器上程序**根本起不来**：
// 动态链接器在 main 之前就拒绝加载，界面、报错、一行日志都不会有。
//
// 所以现在改成 dlopen 依次试探，用系统自带的那一份，不往包里塞 OpenSSL。
// 用到的这 9 个 EVP 符号在 1.0 / 1.1 / 3.0 之间签名完全一致，
// EVP_CIPHER_CTX 在 1.1 之后是不透明类型，但这里只当指针用，不受影响。
// 一个都加载不到时 aesApply 返回空，protect/unprotect 返回空串，
// 由 ConfigStore 既有的「密码保存失败」/「读取失败」分支接管。
//
// 密钥来源降级链：/etc/machine-id -> /var/lib/dbus/machine-id -> 主机名+home。
//   前两个是 systemd/dbus 的标准位置，麒麟和 UOS 都有；
//   都没有时仍得到稳定的机器绑定值，只是绑定粒度从"机器"降为"用户目录"。
// ---------------------------------------------------------------------------

namespace {

// 密文格式前缀。裸 base64（旧版本）没有前缀，据此区分几代格式。
//   SRT2: —— 密钥只由**公开材料**派生（machine-id + 用户名 + 常量盐）
//   SRT3: —— 密钥里掺入了本机私有密钥文件（只有本人可读）
const char *kPrefix   = "SRT2:";
const char *kPrefixV3 = "SRT3:";
const int   kIvLen  = 16;

// AES 的块大小，三个主版本都一样。常量，不参与 dlopen。
const int kBlockLen = 16;
// EVP_MAX_BLOCK_LENGTH 在 1.0 是 32、1.1 起是 32，这里只用它给输出留余量。
const int kOutPad = 32;

// ---- 运行期加载的 EVP 绑定 -------------------------------------------------
//
// 全部用 void* 当句柄：EVP_CIPHER_CTX 在 1.1 之后是不透明类型，
// 写成 void* 在三个主版本上都成立，编译期不依赖任何 OpenSSL 头文件。
struct Evp {
    void *(*ctxNew)();
    void  (*ctxFree)(void *);
    void *(*aes256cbc)(void);
    int   (*cipherInit)(void *, void *, void *,
                        const unsigned char *, const unsigned char *, int);
    int   (*encUpdate)(void *, unsigned char *, int *,
                       const unsigned char *, int);
    int   (*decUpdate)(void *, unsigned char *, int *,
                       const unsigned char *, int);
    int   (*encFinal)(void *, unsigned char *, int *);
    int   (*decFinal)(void *, unsigned char *, int *);
    bool  ready;
};

template <typename T>
static bool bind(void *h, const char *name, T &slot)
{
    void *p = dlsym(h, name);
    if (!p) {
        return false;
    }
    slot = reinterpret_cast<T>(p);
    return true;
}

// 依次试探三个主版本。任何一次全符号绑定成功就返回 true 并缓存。
//
// 缓存失败结果也要缓存：一个连 libcrypto 都找不到的系统，不该每次保存
// 密码都重试三次 dlopen —— 那是在用户点按钮的关键路径上。
//
// 出参 evp 必须被真正填上。**第一版只往静态 cached 里写、没往 evp 拷**，
// 于是返回 true 而调用方拿到的是未初始化的野函数指针：
//   loadEvp -> 1 ctxNew=0x26e9d8c0 aes=0x7d8dfdaf7a98   <- 看着像指针的垃圾
//   aesApply 直接段错误
// 「返回 true」和「出参已填」是两件事，缺一个检查就是随机崩溃。
bool loadEvp(Evp &evp)
{
    static Evp  cached;
    static bool tried = false;
    if (tried) {
        if (cached.ready) {
            evp = cached;
        }
        return cached.ready;
    }
    tried = true;

    // Test hook.  The whole point of the dlopen change is that a system with
    // no usable libcrypto must degrade instead of failing to launch -- and
    // that path is otherwise unreachable on any machine that HAS libcrypto,
    // i.e. exactly where the regression would hide.  This is the only way to
    // prove the graceful branch actually runs.
    if (qgetenv("SRT_NO_CRYPTO") != nullptr) {
        cached.ready = false;
        return false;
    }

    static const char *candidates[] = {
        "libcrypto.so.3", "libcrypto.so.1.1", "libcrypto.so.1.0.0",
        "libcrypto.so",   // 开发环境里的未版本化符号链接，最后兜底
    };
    for (const char *so : candidates) {
        void *h = dlopen(so, RTLD_LAZY | RTLD_LOCAL);
        if (!h) {
            continue;
        }
        Evp t;
        const bool ok =
            bind(h, "EVP_CIPHER_CTX_new", t.ctxNew) &&
            bind(h, "EVP_CIPHER_CTX_free", t.ctxFree) &&
            bind(h, "EVP_aes_256_cbc", t.aes256cbc) &&
            bind(h, "EVP_CipherInit_ex", t.cipherInit) &&
            bind(h, "EVP_EncryptUpdate", t.encUpdate) &&
            bind(h, "EVP_DecryptUpdate", t.decUpdate) &&
            bind(h, "EVP_EncryptFinal_ex", t.encFinal) &&
            bind(h, "EVP_DecryptFinal_ex", t.decFinal);
        if (ok) {
            t.ready = true;
            cached  = t;
            evp     = t;
            // 不 dlclose：卸载会把函数指针变成悬垂，且这条路径一生只跑一次。
            return true;
        }
        dlclose(h);
    }
    cached.ready = false;
    return false;
}

// 16 字节随机数。
//
// 踩过的坑：最初写的是 QByteArray::fromHex(QUuid::createUuid().toRfc4122())。
// 在拿不到系统随机源的环境里（qemu-user 模拟、某些国产系统的容器运行时、
// /dev/urandom 权限被收紧时），createUuid() 返回 **null UUID**，
// toRfc4122() 返回**空串**，fromHex("") 得到 0 字节 IV。
// 接下来 EVP_EncryptInit_ex 会按 AES-CBC 的固定 16 字节去读那块指针，
// 越界读到栈上的随机内存当 IV ——
//   * 加密照样"成功"，S3/ciphertext-varies 还因为每次读到的垃圾不同而 PASS；
//   * 解密必然失败，因为解密读到的又是另一块垃圾。
// 症状是"密码存进去就再也读不出来"，而且没有任何报错。
// 所以这里不依赖 QUuid 的可用性：优先直接读 /dev/urandom，
// 读不到再退到 toByteArray()（它**恒为 16 字节**，与随机源是否可用无关）。
QByteArray randomIv()
{
    QByteArray iv(kIvLen, '\0');
    QFile f(QStringLiteral("/dev/urandom"));
    if (f.open(QIODevice::ReadOnly)) {
        const qint64 got = f.read(iv.data(), kIvLen);
        f.close();
        if (got == kIvLen) {
            return iv;
        }
    }
    // toByteArray() 永远返回 16 字节；即使是 null UUID 也是 16 个 0，
    // 至少不会把长度不足的指针交给 OpenSSL。
    const QByteArray u = QUuid::createUuid().toByteArray();
    for (int i = 0; i < kIvLen; ++i) {
        iv[i] = (i < u.size()) ? u.at(i) : '\0';
    }
    return iv;
}

// 当前登录用户名。
// 不能用 qgetenv("USER")/QProcessEnvironment::userName()（Qt 5.10 才有）：
// 环境变量会随启动方式变化——桌面会话、ssh -X、sudo -H 三种入口的 USER/HOME
// 可以完全不同，同一个用户两次启动就会算出两把不同的密钥，
// 表现为"昨天存的密码今天读不出来"。getpwuid 直接问系统"这个 uid 是谁"，
// 与环境变量无关。
QByteArray currentUser()
{
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_name) {
        return QByteArray(pw->pw_name);
    }
    return QByteArray();
}

QByteArray machineSecret()
{
    static QByteArray cached;
    static bool        inited = false;
    if (inited) {
        return cached;
    }
    inited = true;

    QByteArray id;
    static const char *paths[] = { "/etc/machine-id", "/var/lib/dbus/machine-id" };
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        QFile f(QString::fromLatin1(paths[i]));
        if (f.open(QIODevice::ReadOnly)) {
            id = f.readAll().trimmed();
            f.close();
            if (!id.isEmpty()) {
                break;
            }
        }
    }
    if (id.isEmpty()) {
        id = QSysInfo::machineHostName().toUtf8();
        id += '\x1f';
        id += QDir::homePath().toUtf8();
    }

    // 0x1f 分隔：machine-id / 主机名里不会出现这个字节，
    // 避免拼接歧义（"ab"+"c" 与 "a"+"bc" 拼出同一串）。
    QByteArray material = id;
    material += '\x1f';
    material += currentUser();
    material += '\x1f';
    material += QByteArrayLiteral("SyncReplayTool/secret/v1");

    cached = QCryptographicHash::hash(material, QCryptographicHash::Sha256);
    return cached;
}

// 本机私有密钥：一份随机字节，存成一个**只有本人可读**的文件。
//
// 为什么必须有它（2026-10-21 复核）：
// machineSecret() 的材料是 /etc/machine-id（0444，全机可读）+ 登录用户名
// （任何进程都查得到）+ 一个写死在源码里的常量盐。
// 这三样**全都是公开信息**，所以任何拿到 ini 的人都能离线复算出同一把
// AES 密钥，把 target/password 里的密文还原成目标机的 **SSH 明文口令**。
// "换机器解不开"这个保证只对"没拿到 ini"的人成立，对"拿到 ini"的人不成立。
//
// 加一份只有本人可读的随机材料之后，拿到 ini 但拿不到密钥文件的人
// （同机别的用户）就还原不出口令了 —— 这正是 Windows 上 DPAPI 提供的
// 那一级保证。
//
// 放在 ini 旁边：ini 所在目录本来就是当前用户自己的；
// SRT_CONFIG_DIR 沙箱把两者一起搬走，沙箱内外不会串。
//
// 读不出来 / 建不了文件时返回空串 —— 上层会退回只用公开材料的那套密钥，
// 也就是"保护弱一点但功能正常"，而不是"密码存不进去"。
QByteArray privateKeyMaterial()
{
    static QByteArray cached;
    static bool        inited = false;
    if (inited) {
        return cached;
    }
    inited = true;

    const QString iniPath = ConfigStore::iniPath();
    if (iniPath.isEmpty()) {
        return cached;
    }
    const QString keyPath =
        QFileInfo(iniPath).absolutePath() + QStringLiteral("/SyncReplayTool.key");
    // 目录可能还没建（第一次运行时 ini 还没写过）。建不出来就当没有这一层保护。
    if (!QDir().mkpath(QFileInfo(iniPath).absolutePath())) {
        return cached;
    }

    QFile in(keyPath);
    if (in.open(QIODevice::ReadOnly)) {
        // 密钥材料是 32 个**随机二进制字节**，不能先 trim 再判长度。
        // 判定的口径统一放在 keyMaterialFromBytes() 里（两个平台同一份），
        // 自检也直接调它 —— 免得「判据说一套、读路径做另一套」。
        cached = SecretStore::keyMaterialFromBytes(in.readAll());
        in.close();
    }
    if (cached.size() == 32) {
        return cached;
    }

    // 没有 / 长度不对：生成一份新的。
    QByteArray key(32, '\0');
    QFile rnd(QStringLiteral("/dev/urandom"));
    bool got = false;
    if (rnd.open(QIODevice::ReadOnly)) {
        got = (rnd.read(key.data(), 32) == 32);
        rnd.close();
    }
    if (!got) {
        const QByteArray u = QUuid::createUuid().toByteArray();
        for (int i = 0; i < 32; ++i)
            key[i] = (i < u.size()) ? u.at(i) : '\0';
    }

    QFile out(keyPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return cached;                 // 建不了就当没有这一层保护
    }
    // 权限**先收紧，再写内容**。
    //
    // 原来顺序反了：先 write 再 setPermissions ⇒ 这 32 字节密钥材料
    // 在 umask 权限（常见 022 ⇒ 0644，全机可读）下有一个窗口，
    // 同机别的用户趁窗口把密钥文件拷走 ⇒ 拿到 ini 的人就能离线复算出口令
    // —— 那正是本文件存在的意义要防的事。
    // 现在先收紧到 0600（此时文件还是空的，窗口里没有密钥材料），再写。
    //
    // 收紧失败时（某些网络盘/FAT 挂载不支持 chmod）仍然要写：
    // **不能**因为收紧失败就不写这个文件 —— 不写等于上一层保护整层失效，
    // 密钥版本会变，客户已有的密文解不开。那比"权限没收紧"严重得多。
    // 这里刻意不改变失败时的行为，只把**窗口里没有密钥材料**这件事做到。
    //
    // Windows 上 setPermissions 只映射到只读属性、改不了 ACL
    // （那边靠 DPAPI，不靠这个文件），Linux 上是真的生效。
    const bool tightened = out.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    Q_UNUSED(tightened);
    out.write(key);
    out.close();
    cached = key;
    return cached;
}

// 掺入私有材料之后的密钥。
QByteArray machineSecretV3()
{
    static QByteArray cached;
    static bool        inited = false;
    if (inited) {
        return cached;
    }
    inited = true;

    const QByteArray priv = privateKeyMaterial();
    if (priv.isEmpty()) {
        return cached;                 // 没有私有材料 -> 调用方退回 v1
    }
    QByteArray material = machineSecret();
    material += '\x1f';
    material += priv;
    cached = QCryptographicHash::hash(material, QCryptographicHash::Sha256);
    return cached;
}

QByteArray aesApply(bool encrypt, const QByteArray &key, const QByteArray &iv,
                    const QByteArray &in)
{
    // AES-CBC 的 key 和 IV 是**固定长度**参数，OpenSSL 不会检查你给的缓冲够不够，
    // 会直接按 16/32 字节去读。长度不对就是静默越界读栈内存——
    // 加密照样"成功"，解密必然失败，密码存进去再也读不出来。
    // 所以这两道检查不是防御性冗余，是必须的。
    if (key.size() != 32 || iv.size() != kIvLen) {
        return QByteArray();
    }

    Evp evp;
    // 系统里没有任何一个可用的 libcrypto：返回空，交给上层的失败分支，
    // 而不是让程序在加载期就死掉、连一句报错都留不下。
    if (!loadEvp(evp)) {
        return QByteArray();
    }

    void *ctx = evp.ctxNew();
    if (!ctx) {
        return QByteArray();
    }

    QByteArray out;
    out.resize(in.size() + kOutPad);
    int  len   = 0;
    int  total = 0;
    bool ok    = false;

    if (evp.cipherInit(ctx, evp.aes256cbc(), nullptr,
                       reinterpret_cast<const unsigned char *>(key.constData()),
                       reinterpret_cast<const unsigned char *>(iv.constData()),
                       encrypt ? 1 : 0) == 1) {
        const unsigned char *inp =
            reinterpret_cast<const unsigned char *>(in.constData());
        unsigned char *outp = reinterpret_cast<unsigned char *>(out.data());

        if (encrypt) {
            ok = evp.encUpdate(ctx, outp, &len, inp, static_cast<int>(in.size())) == 1;
        } else {
            ok = evp.decUpdate(ctx, outp, &len, inp, static_cast<int>(in.size())) == 1;
        }
        if (ok) {
            total = len;
            if (encrypt) {
                ok = evp.encFinal(ctx, outp + total, &len) == 1;
            } else {
                ok = evp.decFinal(ctx, outp + total, &len) == 1;
            }
            total += len;
        }
    }

    evp.ctxFree(ctx);
    if (!ok) {
        return QByteArray();
    }
    return out.left(total);
}

} // namespace

bool SecretStore::isEncrypted() { return true; }

// 此刻这台机器上到底能不能加密：就是问一次 dlopen 有没有成功。
//
// loadEvp 内部会缓存结果，重复问不会重复 dlopen。
bool SecretStore::cryptoBackendAvailable()
{
    Evp evp;
    return loadEvp(evp);
}

QString SecretStore::backendName()
{
    // 界面上不要写死"DPAPI"——Linux 上没有 DPAPI，
    // 沿用旧文案会显示一条不存在的保护手段，比不显示更糟。
    return QString::fromUtf8(u8"\u672C\u673A\u6D3E\u751F\u5BC6\u94A5");
}

QString SecretStore::protect(const QString &plain)
{
    if (plain.isEmpty()) {
        return QString();
    }
    const QByteArray iv = randomIv();
    // 优先用掺了私有材料的密钥（v3）。私有材料拿不到时退回 v1 ——
    // 保护弱一档，但密码照样能存能取，不给用户"存不进去"的dead end。
    QByteArray key = machineSecretV3();
    const char *prefix = kPrefixV3;
    if (key.isEmpty()) {
        key = machineSecret();
        prefix = kPrefix;
    }
    const QByteArray ct = aesApply(true, key, iv, plain.toUtf8());
    if (ct.isEmpty()) {
        return QString();
    }
    return QString::fromLatin1(QByteArray(prefix) + (iv + ct).toBase64());
}

QString SecretStore::unprotect(const QString &stored)
{
    if (stored.isEmpty()) {
        return QString();
    }

    // 新格式：密钥里掺了本机私有材料。
    if (stored.startsWith(QString::fromLatin1(kPrefixV3))) {
        const QByteArray key = machineSecretV3();
        if (key.isEmpty()) {
            return QString();
        }
        const QByteArray raw = QByteArray::fromBase64(stored.mid(5).toLatin1());
        if (raw.size() <= kIvLen) {
            return QString();
        }
        const QByteArray pt =
            aesApply(false, key, raw.left(kIvLen), raw.mid(kIvLen));
        if (pt.isEmpty()) {
            return QString();
        }
        const QString plain = QString::fromUtf8(pt);
        if (plain.toUtf8() != pt)
            return QString();
        return plain;
    }

    // 上一代格式：密钥只由公开材料派生。仍然要能解开 ——
    // 老配置里的口令不能因为程序升级就再也读不出来。
    if (stored.startsWith(QString::fromLatin1(kPrefix))) {
        const QByteArray raw = QByteArray::fromBase64(stored.mid(5).toLatin1());
        if (raw.size() <= kIvLen) {
            return QString();
        }
        const QByteArray pt =
            aesApply(false, machineSecret(), raw.left(kIvLen), raw.mid(kIvLen));
        // 解不开（换了机器/用户）返回空串：ConfigStore 会据此提示
        // "读取已保存的密码失败"，而不是静默当成空密码把原密文覆盖掉。
        if (pt.isEmpty()) {
            return QString();
        }
        // 往返校验：口令一定是文本，解出来的字节必须仍是合法 UTF-8 才认。
        //
        // 少了这一条，ini 被拷到另一台机器（密钥不同）时，
        // AES-CBC 解出来的**垃圾字节**只要 padding 恰好通过（约 1/256）
        // 就会被当成口令返回 —— 用户看到的是一串乱码，认证失败，
        // 而"密文本机解不开"这条真正的线索不会出现在界面上，
        // 反而绕开了 ConfigStore 专门准备的提示。
        //
        // 旧 base64 分支一直有这道校验（见下面），两条路径的口径必须一致。
        const QString plain = QString::fromUtf8(pt);
        if (plain.toUtf8() != pt)
            return QString();
        return plain;
    }

    // 旧版本的裸 base64（无前缀）。继续按 base64 解，
    // 否则升级后用户已保存的口令会全部变成"读取失败"。
    //
    // 但**不能无条件接受解出来的结果**。QByteArray::fromBase64 对任何字符串
    // 都会给出一个字节序列（非法字符被直接丢掉），所以配置一旦损坏——写到一半
    // 断电、同步冲突、换了编辑器——这里就会把一串垃圾字节当成有效口令返回。
    // 用户看到的现象是"登录失败"而不是"配置读不出来"，真正的口令就这么被垃圾
    // 顶掉了，而且没有任何提示。
    //
    // 实测（arm64 / qemu-aarch64）：ini 里写 `!!!not base64 at all!!!`，
    // 解出来是一串非空乱码，被当成口令用了。Windows 的 DPAPI 分支不会这样
    // （解不开就返回空），所以这个洞只在 Linux 上。
    //
    // 口令一定是文本，解出来的字节必须仍是合法 UTF-8 才认。
    // 往返比对（toUtf8 之后必须与原字节完全一致）能同时挡住非法序列、
    // 超长编码、孤立代理项这几类"看着像文本"的坏字节。
    const QByteArray legacy = QByteArray::fromBase64(stored.toLatin1());
    if (legacy.isEmpty())
        return QString();
    const QString plain = QString::fromUtf8(legacy);
    if (plain.toUtf8() != legacy)
        return QString();
    return plain;
}

#endif

// ---------------------------------------------------------------------------
// 从磁盘读回来的字节里认出「这就是密钥材料」。
//
// 见 SecretStore.h 里的说明：这里**按字节原样**判 32 字节，
// 只有在长度不对时才退一步按「早期可能写下的文本形态」去掉首尾空白再试。
//
// 为什么单独成函数（而不是就地 trim）：自检要能**直接**调它做阳性/阴性对照；
// 而且这段逻辑两个平台都要对，不能各写一份。
QByteArray SecretStore::keyMaterialFromBytes(const QByteArray &raw)
{
    if (raw.size() == 32)
        return raw;                       // 正常形态：32 个随机字节，原样收下
    const QByteArray legacy = raw.trimmed();
    if (legacy.size() == 32)
        return legacy;                    // 早期文本形态的兼容（去空白后正好 32 字节）
    return QByteArray();                  // 不是密钥材料
}
