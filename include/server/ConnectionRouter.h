// 连接路由表
// 维护 fd 到所属 Reactor 的映射关系
// 当需要跨 Reactor 发送消息时通过查这个表找到目标 Reactor
// 由于可能被多个线程同时访问，内部使用互斥锁保护
#pragma once

#include <unordered_map>
#include <mutex>

class Reactor;

class ConnectionRouter {
public:
    // 记录某个 fd 归属于哪个 Reactor
    void register_connection(int fd, Reactor* reactor);

    // 删除某个 fd 的路由记录
    void unregister_connection(int fd);

    // 查询某个 fd 归属于哪个 Reactor
    // 如果查询不到返回 nullptr
    Reactor* get_reactor(int fd);

private:
    // fd 到 Reactor 指针的映射表
    std::unordered_map<int, Reactor*> map_;

    // 保护映射表的互斥锁
    // 因为注册和查询可能来自不同的 Reactor 线程
    mutable std::mutex mtx_;
};
