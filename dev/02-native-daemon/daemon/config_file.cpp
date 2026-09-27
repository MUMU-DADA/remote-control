// config_file.cpp

#include "config_file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "autod_log.h"
#include "websocket.h"   // Base64Encode

namespace autod {
namespace {

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' ||
                     s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

bool ParseBool(const std::string& v, bool def) {
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return def;
}

}  // namespace

std::string ConfigFile::DefaultPath() {
    const char* env = getenv("AUTOD_CONFIG");
    if (env != nullptr && *env != '\0') return env;
    return "/sdcard/autod.conf";
}

bool ConfigFile::Load(const std::string& path, PersistedConfig* out,
                      std::string* error) {
    *out = PersistedConfig{};

    FILE* f = fopen(path.c_str(), "re");
    if (f == nullptr) {
        if (errno == ENOENT) {
            // 首启就是这个状态。不是错误 —— 用默认值（无鉴权、8088、启用）。
            ALOGI("配置文件 %s 不存在，使用默认配置（无鉴权）", path.c_str());
            return true;
        }
        if (error) {
            *error = "打开 " + path + " 失败: " + strerror(errno);
        }
        return false;
    }

    char line[1024];
    int lineno = 0;
    while (fgets(line, sizeof(line), f) != nullptr) {
        ++lineno;
        std::string s = Trim(line);
        if (s.empty() || s[0] == '#' || s[0] == ';') continue;

        const size_t eq = s.find('=');
        if (eq == std::string::npos) {
            ALOGW("%s:%d 不是 key=value，跳过: %s", path.c_str(), lineno, s.c_str());
            continue;
        }
        const std::string key = Trim(s.substr(0, eq));
        const std::string val = Trim(s.substr(eq + 1));

        if (key == "enabled") {
            out->enabled = ParseBool(val, true);
        } else if (key == "port") {
            char* end = nullptr;
            const long v = strtol(val.c_str(), &end, 10);
            if (end != nullptr && *end == '\0' && v > 0 && v <= 65535) {
                out->port = static_cast<int>(v);
            } else {
                ALOGW("%s:%d 端口不合法(%s)，忽略", path.c_str(), lineno, val.c_str());
            }
        } else if (key == "bind") {
            out->bind = val.empty() ? "127.0.0.1" : val;
        } else if (key == "auth") {
            out->auth = ParseBool(val, false);
        } else if (key == "token") {
            out->token = val;
        } else {
            ALOGW("%s:%d 未知字段 %s（忽略）", path.c_str(), lineno, key.c_str());
        }
    }
    fclose(f);
    return true;
}

std::string ConfigFile::Serialize(const PersistedConfig& cfg) {
    std::string s;
    s += "# autod 配置 —— 由上位应用或手工编辑，守护进程启动时读取。\n";
    s += "# 改完之后需要重启服务才生效。\n";
    s += "\n";
    s += "# 服务是否应当运行\n";
    s += "enabled=" + std::string(cfg.enabled ? "1" : "0") + "\n";
    s += "\n";
    s += "# 监听地址。127.0.0.1 = 仅本机；0.0.0.0 = 对外（注意鉴权设置）\n";
    s += "bind=" + cfg.bind + "\n";
    s += "\n";
    s += "# HTTP 监听端口\n";
    s += "port=" + std::to_string(cfg.port) + "\n";
    s += "\n";
    s += "# 是否要求访问令牌。0 = 无鉴权（任何人都能访问接口）\n";
    s += "auth=" + std::string(cfg.auth ? "1" : "0") + "\n";
    s += "\n";
    s += "# 访问令牌。auth=1 时若为空，守护进程启动时会随机生成并写回这里。\n";
    s += "token=" + cfg.token + "\n";
    return s;
}

bool ConfigFile::Save(const std::string& path, const PersistedConfig& cfg,
                      std::string* error) {
    // 原子替换：先写临时文件再 rename。
    // 直接覆写的话，上位应用可能在守护进程写到一半时读到半个文件。
    const std::string tmp = path + ".tmp";
    const std::string body = Serialize(cfg);

    // 加 O_CLOEXEC：这是 root 进程，别把这个 fd 泄漏给子进程
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
    if (fd < 0) {
        if (error) *error = "写 " + tmp + " 失败: " + strerror(errno);
        return false;
    }
    size_t sent = 0;
    while (sent < body.size()) {
        const ssize_t n = write(fd, body.data() + sent, body.size() - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (error) *error = std::string("写入失败: ") + strerror(errno);
            close(fd);
            unlink(tmp.c_str());
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    // fsync 再 rename：不然掉电后可能 rename 成功但内容还没落盘
    fsync(fd);
    close(fd);

    if (rename(tmp.c_str(), path.c_str()) != 0) {
        if (error) *error = "替换 " + path + " 失败: " + strerror(errno);
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

std::string ConfigFile::GenerateToken(size_t bytes) {
    // /dev/urandom 而不是 rand()：这个令牌是唯一的访问控制手段，
    // 可预测的令牌等于没有。
    const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        ALOGE("打不开 /dev/urandom: %s —— 拒绝生成令牌", strerror(errno));
        return {};
    }
    std::string raw(bytes, '\0');
    size_t got = 0;
    while (got < bytes) {
        const ssize_t n = read(fd, &raw[got], bytes - got);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            ALOGE("读 /dev/urandom 失败: %s", strerror(errno));
            return {};
        }
        if (n == 0) break;
        got += static_cast<size_t>(n);
    }
    close(fd);
    if (got != bytes) return {};

    // base64url：把 +/ 换成 -_ 并去掉 padding。
    // 令牌会出现在 URL 和命令行里，这两个字符在那些地方需要转义。
    std::string b64 = Base64Encode(raw);
    std::string out;
    out.reserve(b64.size());
    for (char c : b64) {
        if (c == '+')      out += '-';
        else if (c == '/') out += '_';
        else if (c == '=') continue;
        else               out += c;
    }
    return out;
}

}  // namespace autod
