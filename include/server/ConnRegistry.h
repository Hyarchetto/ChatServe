// 线程安全的 fd → Connection 唯一连接表 持有所有权
// 供 IO 线程注册销毁 供线程池 worker 将 fd 解析为 shared_ptr
// 解决 fd 重用竞态：IO 线程创建 IO action 时用的是解析后的 shared_ptr 而非原始 fd
#pragma once

#include <mutex>
#include <unordered_map>
#include <memory>

class Connection;

struct ConnRegistry {
    std::mutex mtx_;
    std::unordered_map<int, std::shared_ptr<Connection>> map_;

    void add(int fd, std::shared_ptr<Connection> conn) {
        std::lock_guard<std::mutex> lock(mtx_);
        map_[fd] = std::move(conn);
    }

    void remove(int fd) {
        std::lock_guard<std::mutex> lock(mtx_);
        map_.erase(fd);
    }

    // 返回 shared_ptr 拷贝 连接已断开 alive_ 为 false 则返回 nullptr
    std::shared_ptr<Connection> get(int fd) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = map_.find(fd);
        if (it == map_.end()) return nullptr;
        if (!it->second->alive_) return nullptr;
        return it->second;
    }
};
