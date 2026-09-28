// peer_util.h — TCP 对端地址
//
// 存在的理由：抓帧节奏是「所有订阅者的最高需求」，所以只要有一个客户端
// 挂着 60fps，整个进程就一直在满速抓帧。看不到"是谁在拉"的话，
// 排查就只能靠挨个关客户端试。
//
// 做成 header-only 是为了不动 AOSP 那边的源文件清单
// （tools/integrate-aosp.sh 里是逐个列出来的）。

#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <string>

namespace remote_control {

// 返回 "ip:port"；拿不到地址（AF_UNIX / 已断开）时返回空串。
//
// ⚠️ 空串是**正常情况**，不是错误 —— socket 那条传输就没有对端地址。
//    调用方不要把它当失败。
inline std::string PeerName(int fd) {
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&ss), &len) != 0) {
        return {};
    }

    char host[INET6_ADDRSTRLEN] = {0};
    uint16_t port = 0;

    if (ss.ss_family == AF_INET) {
        auto* a = reinterpret_cast<sockaddr_in*>(&ss);
        if (inet_ntop(AF_INET, &a->sin_addr, host, sizeof(host)) == nullptr) {
            return {};
        }
        port = ntohs(a->sin_port);
    } else if (ss.ss_family == AF_INET6) {
        auto* a = reinterpret_cast<sockaddr_in6*>(&ss);
        if (inet_ntop(AF_INET6, &a->sin6_addr, host, sizeof(host)) == nullptr) {
            return {};
        }
        port = ntohs(a->sin6_port);
    } else {
        return {};
    }

    return std::string(host) + ":" + std::to_string(port);
}

}  // namespace remote_control
