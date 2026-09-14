// 事件循环的实现
// 把 EventLoop 作为成员变量嵌入Reactor
#include "core/EventLoop.h"

#include <cerrno>
#include <cstdio>
#include <thread>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

// 当前线程正在运行的事件循环 loop 入口登记 邮箱据此判断本地直投还是跨线程投递
static thread_local EventLoop* t_loop = nullptr;

// 构造函数什么都不做，真正的初始化工作由 init 函数完成
// 在容器初始化完毕后再调用 init
EventLoop::EventLoop() {}

// 析构函数关闭 epoll 和 eventfd
EventLoop::~EventLoop() {
    if (this->epollfd_ >= 0) {
        close(this->epollfd_);
    }
    if (this->eventfd_ >= 0) {
        close(this->eventfd_);
    }
}

// 初始化 EventLoop 的两个核心句柄
bool EventLoop::init() {
    this->epollfd_ = epoll_create(1);
    if (this->epollfd_ < 0) {
        perror("epoll_create");
        return false;
    }
    this->eventfd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (this->eventfd_ < 0) {
        perror("eventfd");
        close(this->epollfd_);
        this->epollfd_ = -1;
        return false;
    }
    this->add_event(this->eventfd_, EPOLLIN, [this]() { this->handle_eventfd();});
    return true;
}

// 事件循环主函数
void EventLoop::loop() {
    t_loop = this;
    std::vector<epoll_event> evs(kMaxEvents);

    while (!this->quit_) {
        int n = epoll_wait(this->epollfd_, evs.data(), kMaxEvents, -1);
        // epoll error 处理
        if (n < 0) {
            if (errno != EINTR){
                perror("epoll_wait");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            continue;
        }
        for (int i = 0; i < n; ++i) {
            int fd = evs[i].data.fd;

            auto it = this->event_map_.find(fd);
            if (it == this->event_map_.end()) continue;

            uint32_t flags = evs[i].events;

            // 快照回调，避免执行过程中 event_map_ 被修改迭代器失效导致指针越界
            auto read_cb = it->second.read_cb_;
            auto write_cb = it->second.write_cb_;
            auto err_cb = it->second.err_cb_;

            // 读优先 干净关闭以 EPOLLIN 呈现 recv 返回 0 即关闭
            if (flags & EPOLLIN && read_cb) {
                read_cb();
            }
            // 对端挂断 EPOLLHUP 连接错误 EPOLLERR 都走关闭回调
            // 读路径已关闭连接时 fd 已摘除 用 event_map_ 判活避免重复清理
            if (flags & (EPOLLERR | EPOLLHUP) && err_cb && this->event_map_.count(fd)) {
                err_cb();
            }
            if (flags & EPOLLOUT && write_cb && this->event_map_.count(fd)) {
                write_cb();
            }
        }
    }
    t_loop = nullptr;
}

// 设置退出标志并唤醒 epoll_wait
// 确保 loop 函数能尽快检测到 quit_ 的变化并退出
void EventLoop::quit() {
    this->quit_ = true;
    this->wakeup();
}

// 把一个句柄及其回调注册到 epoll 中
// 先挂进 epoll 再留回调，挂失败就什么都不留，避免留下收不到事件的空转条目
bool EventLoop::add_event(int fd, uint32_t events,
                          std::function<void()> read_cb,
                          std::function<void()> write_cb,
                          std::function<void()> err_cb) {
    epoll_event ev{};
    ev.data.fd = fd;
    ev.events = events;

    if (epoll_ctl(this->epollfd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        perror("epoll_ctl ADD");
        return false;
    }
    this->event_map_.insert_or_assign(fd, EventCallbacks{std::move(read_cb),
                                                         std::move(write_cb),
                                                         std::move(err_cb)});
    return true;
}

// 删除一个句柄的监听
// 去掉监听失败说明 epoll 里本就没有该 fd，忽略即可，回调必须摘掉
void EventLoop::del_event(int fd) {
    this->event_map_.erase(fd);
    if (epoll_ctl(this->epollfd_, EPOLL_CTL_DEL, fd, NULL) < 0 && errno != ENOENT) {
        perror("epoll_ctl DEL");
    }
}

// 修改一个句柄在 epoll 中的监听事件
bool EventLoop::mod_event(int fd, uint32_t events) {
    epoll_event ev{};
    ev.data.fd = fd;
    ev.events = events;
    if (epoll_ctl(this->epollfd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
        perror("epoll_ctl MOD");
        return false;
    }
    return true;
}

// 判断当前线程是否本事件循环线程
bool EventLoop::is_in_loop_thread() const {
    return t_loop == this;
}

// 入队后写 eventfd 唤醒 epoll_wait 取件 不判线程 本 loop 线程调也只是入队
void EventLoop::post(std::function<void()> cb) {
    {
        std::lock_guard<std::mutex> lock(this->mtx_functors_);
        this->pending_functors_.push_back(std::move(cb));
    }
    this->wakeup();
}

// 写入 eventfd 来唤醒 epoll_wait
void EventLoop::wakeup() {
    // init 前或 init 失败时 eventfd_ 为 -1 信号早到路径直接跳过避免 EBADF 噪音
    if (this->eventfd_ < 0) {
        return;
    }
    uint64_t x = 1;
    if (write(this->eventfd_, &x, sizeof(x)) < 0) {
        perror("write eventfd");
    }
}

// 响应事件回调
void EventLoop::handle_eventfd() {
    // 先读取 eventfd 的数据
    uint64_t x;
    if (read(this->eventfd_, &x, sizeof(x)) < 0 && errno != EAGAIN) {
        perror("read eventfd");
    }
    // 然后处理线程池回调
    this->do_pending_functors();
}

// 执行所有待办回调
void EventLoop::do_pending_functors() {
    std::vector<std::function<void()>> functors;
    {
        // 先加锁把 pending_functors_ 的数据交换到局部变量中，减少锁的持有时间
        std::lock_guard<std::mutex> lock(this->mtx_functors_);
        functors.swap(this->pending_functors_);
    }
    // 完成所有剩余回调函数
    for (auto& f : functors) {
        f();
    }
}
