// 事件循环
// 封装了 epoll 和跨线程任务投递机制，不包含任何业务
// 每个 EventLoop 独占一个线程，无限循环执行 epoll_wait
// 其他线程可以通过 run_in_loop 把任务安全地塞进本循环
#pragma once

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <thread>
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

    // 把一个回调函数投递到事件循环线程中去执行
    // 如果调用方已经在事件循环线程中，则直接执行
    // 否则把回调加入队列并写入 eventfd 唤醒 epoll_wait
    void run_in_loop(std::function<void()> cb);

    // 写入 eventfd 来唤醒 epoll_wait 使之立刻返回
    void wakeup();

    // 当前线程是否是事件循环所在的线程
    bool is_in_loop_thread() const;

    // 返回待执行回调队列的长度
    // 这个值是一个近似数量，用于给 MainReactor 做负载感知分发，
    // 可以看出哪个子 Reactor 当前最空闲
    size_t pending_size() const;

private:
    int epollfd_ = -1;           // epoll 实例的文件描述符
    int eventfd_ = -1;           // eventfd 用于跨线程唤醒

    // 每个 fd 关联的三个回调：读、写、错误
    struct EventCallbacks {
        std::function<void()> readCb_;
        std::function<void()> writeCb_;
        std::function<void()> errCb_;
    };

    // fd 到其三个回调的映射关系表
    std::unordered_map<int, EventCallbacks> event_map_;

    // 保护 pending_functors_ 的互斥锁
    std::mutex mtx_functors_;

    // 其他线程通过 run_in_loop 投递过来的回调队列
    std::vector<std::function<void()>> pending_functors_;

    std::thread::id loop_tid_;   // 事件循环所在线程的 ID
    bool quit_ = false;          // 退出标志，loop 函数每轮都会检查

    // 读取 eventfd 中的数据，清空唤醒标记
    void handle_eventfd();

    // 把 pending_functors_ 中的回调全部取出并执行
    void do_pending_functors();

    static constexpr int MAX_EVENTS = 1024;
};
