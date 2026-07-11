// 连接路由表的实现
// 这个路由表维护了 fd 到 Reactor 的映射关系
// 主要用途是在 WebSocket 广播消息时，
// 房间里的人可能分布在不同的子 Reactor 上，
// 需要查这个表才能知道每个 fd 归哪个 Reactor 管理
#include "server/ConnectionRouter.h"

// 注册一个连接的路由记录
// 告知路由表：某个 fd 由哪个 Reactor 负责管理
// 这个函数通常在子 Reactor 的 add_connection 中被调用
void ConnectionRouter::register_connection(int fd, Reactor* reactor) {
    std::lock_guard<std::mutex> lock(this->mtx_);
    this->map_[fd] = reactor;
}

// 删除一个连接的路由记录
// 在连接关闭时调用，避免路由指向已关闭的 fd
void ConnectionRouter::unregister_connection(int fd) {
    std::lock_guard<std::mutex> lock(this->mtx_);
    this->map_.erase(fd);
}

// 查询某个连接属于哪个 Reactor
// 如果查询不到返回 nullptr
// 这个函数可能被任意子 Reactor 的程序路径调用，
// 所以加锁保护
Reactor* ConnectionRouter::get_reactor(int fd) {
    std::lock_guard<std::mutex> lock(this->mtx_);
    auto it = this->map_.find(fd);
    return it != this->map_.end() ? it->second : nullptr;
}
