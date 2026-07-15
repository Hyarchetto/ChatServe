// 事件循环
// 封装了 epoll 和跨线程任务投递机制，不包含任何业务
// 每个 EventLoop 独占一个线程，无限循环执行 epoll_wait
// 线程池干完活了就用 run_in_loop 把结果塞回 IO 线程
#pragma once

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <cstdint>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

class EventLoop {
public:
    // 构造时不做任何事情，真正的初始化在 init 中完成
    EventLoop();
    ~EventLoop();

    // ==================== 工作接口 ====================

    // 创建 epoll 句柄和 eventfd，把 eventfd 注册到 epoll 中
    // 必须在调用 loop 之前调用
    bool init();

    // 事件循环主函数
    // 本函数不会返回，直到 quit 被调用
    void loop();

    // 设置退出标志，然后唤醒 epoll_wait 让它尽快返回并退出
    void quit();

    // ==================== epoll 事件注册 ====================

    // 把一个 fd 注册到 epoll 中
    // events 是 EPOLLIN EPOLLOUT EPOLLET 等标志的组合
    // read_cb  为读事件回调
    // write_cb 为写事件回调
    // err_cb   为错误事件回调
    void add_event(int fd, uint32_t events,
                   std::function<void()> read_cb = nullptr,
                   std::function<void()> write_cb = nullptr,
                   std::function<void()> err_cb = nullptr);

    // 从 epoll 中移除一个 fd，同时删除它关联的三个回调
    void del_event(int fd);

    // 修改一个 fd 在 epoll 中的监听事件
    // 比如从只读改成读写，或去掉写事件
    void mod_event(int fd, uint32_t events);

    // 检查某个 fd 是否已经注册到本事件循环
    bool has_event(int fd) const;

    // ==================== 跨线程任务投递 ====================

    // 把一个回调扔到 IO 线程去执行
    // 加锁入队然后 eventfd 叫醒 epoll_wait
    void run_in_loop(std::function<void()> cb);

    // 写入 eventfd 来唤醒 epoll_wait 使之立刻返回
    void wakeup();


private:
    int epollfd_ = -1;           // epoll 实例的文件描述符
    int eventfd_ = -1;           // eventfd 用于跨线程唤醒

    // 每个 fd 关联的三个回调：读、写、错误
    struct EventCallbacks {
        std::function<void()> read_cb_;
        std::function<void()> write_cb_;
        std::function<void()> err_cb_;
    };

    // fd 到其三个回调的映射关系表
    std::unordered_map<int, EventCallbacks> event_map_;

    // 保护 pending_functors_ 的互斥锁
    std::mutex mtx_functors_;

    // 线程池投回来的待办回调队列，handle_eventfd 里会取出来执行
    std::vector<std::function<void()>> pending_functors_;

    bool quit_ = false;          // 退出标志，loop 函数每轮都会检查

    // 读取 eventfd 中的数据，清空唤醒标记
    void handle_eventfd();

    // 把 pending_functors_ 中的回调全部取出并执行
    void do_pending_functors();

    static constexpr int MAX_EVENTS = 1024;
};
