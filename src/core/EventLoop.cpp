// 事件循环的实现
// EventLoop 是整个服务器最基础的 IO 调度单元
// 每个 EventLoop 拥有一个 epoll 实例和一个 eventfd 用来唤醒阻塞的 IO 线程
// 主循环逻辑很简单：epoll_wait 等事件、分发回调、处理线程池投回来的活
#include "core/EventLoop.h"

#include <cstdio>
#include <thread>

// 构造函数什么都不做
// 真正的初始化工作由 init 函数完成
// 这样设计的好处是允许把 EventLoop 作为成员变量嵌入其他类，
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
// 第一步创建 epoll 实例
// 第二步创建 eventfd 并设为非阻塞模式
// 第三步把 eventfd 注册到 epoll 中等待可读事件
// 返回 true 表示初始化成功
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
// 这个函数会一直运行直到 quit_ 被设为 true
//
// 每轮循环的流程：
//   1. epoll_wait 等事件
//   2. 根据事件类型调对应的回调
//      EPOLLERR 调错误回调然后跳过
//      EPOLLIN 调读回调
//      EPOLLOUT 回调：重新查表，防止读回调里删了 fd 导致悬空
//   3. 进入下一轮循环
//
// 线程池投回来的回调在 handle_eventfd 里处理，不在循环底部做了
void EventLoop::loop() {
    std::vector<epoll_event> evs(MAX_EVENTS);

    while (!this->quit_) {
        int n = epoll_wait(this->epollfd_, evs.data(), MAX_EVENTS, -1);
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

            if (flags & EPOLLERR) {
                if (err_cb) err_cb();
                continue;
            }
            if (flags & EPOLLIN && read_cb) {
                read_cb();
            }
            if (flags & EPOLLOUT && write_cb) {
                write_cb();
            }
        }
    }
}

// 设置退出标志并唤醒 epoll_wait
// 确保 loop 函数能尽快检测到 quit_ 的变化并退出
void EventLoop::quit() {
    this->quit_ = true;
    this->wakeup();
}

// 把一个 fd 及其回调注册到 epoll 中
// events 是 epoll 事件标志，通常是 EPOLLIN 或 EPOLLOUT 的组合
// 如果是边缘触发模式，需要加上 EPOLLET
void EventLoop::add_event(int fd, uint32_t events,
                          std::function<void()> read_cb,
                          std::function<void()> write_cb,
                          std::function<void()> err_cb) {
    epoll_event ev{};
    ev.data.fd = fd;
    ev.events = events;

    this->event_map_.insert({fd, {std::move(read_cb),
                                  std::move(write_cb),
                                  std::move(err_cb)}});
    epoll_ctl(this->epollfd_, EPOLL_CTL_ADD, fd, &ev);
}

// 从 epoll 中删除一个 fd 的监听
// 同时从 event_map_ 中删除对应的回调
void EventLoop::del_event(int fd) {
    this->event_map_.erase(fd);
    epoll_ctl(this->epollfd_, EPOLL_CTL_DEL, fd, NULL);
}

// 修改一个 fd 在 epoll 中的监听事件类型
// 例如从监听 EPOLLIN 改为同时监听 EPOLLIN 和 EPOLLOUT
void EventLoop::mod_event(int fd, uint32_t events) {
    epoll_event ev{};
    ev.data.fd = fd;
    ev.events = events;
    epoll_ctl(this->epollfd_, EPOLL_CTL_MOD, fd, &ev);
}

// 检查某个 fd 是否已经注册到本 EventLoop 中
bool EventLoop::has_event(int fd) const {
    return this->event_map_.find(fd) != this->event_map_.end();
}

// 线程池通过这个函数把活投回 IO 线程
void EventLoop::run_in_loop(std::function<void()> cb) {
    {
        std::lock_guard<std::mutex> lock(this->mtx_functors_);
        this->pending_functors_.push_back(std::move(cb));
    }
    this->wakeup();
}

// 写入 eventfd 来唤醒 epoll_wait
void EventLoop::wakeup() {
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
