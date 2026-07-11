// 事件循环的实现
// EventLoop 是整个服务器最基础的 IO 调度单元
// 每个 EventLoop 拥有一个 epoll 实例和一个 eventfd 用于跨线程唤醒
// 主循环逻辑很简单：epoll_wait 等待事件、分发回调、执行跨线程任务
#include "core/EventLoop.h"

#include <cstdio>

// 构造函数什么都不做
// 真正的初始化工作由 init 函数完成
// 这样设计的好处是允许把 EventLoop 作为成员变量嵌入其他类，
// 在容器初始化完毕后再调用 init
EventLoop::EventLoop() {}

// 析构函数关闭 epoll 和 eventfd
EventLoop::~EventLoop() {
    if (this->epollfd_ >= 0) close(this->epollfd_);
    if (this->eventfd_ >= 0) close(this->eventfd_);
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
    this->add_event(this->eventfd_, EPOLLIN,
        [this]() { this->handle_eventfd(); });
    return true;
}

// 事件循环主函数
// 这个函数会一直运行直到 quit_ 被设为 true
//
// 每轮循环的流程：
//   1. 调用 epoll_wait 等待事件发生，超时时间设为 -1 表示无限等待
//   2. 如果有事件发生，根据事件类型调用对应的回调
//      EPOLLERR 调用错误回调并跳过这一轮
//      EPOLLIN 调用读回调
//      EPOLLOUT 调用写回调
//   3. 执行跨线程投递的待办任务
//   4. 进入下一轮循环
//
// 注意 EPOLLOUT 的处理：epoll_wait 返回后重新查找 event_map_，
// 防止在读回调中该 fd 已经被删除导致悬空引用
void EventLoop::loop() {
    this->loop_tid_ = std::this_thread::get_id();
    std::vector<epoll_event> evs(MAX_EVENTS);

    while (!this->quit_) {
        int n = epoll_wait(this->epollfd_, evs.data(), MAX_EVENTS, -1);

        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            continue;
        }

        for (int i = 0; i < n; ++i) {
            int fd = evs[i].data.fd;

            auto it = this->event_map_.find(fd);
            if (it == this->event_map_.end()) continue;

            auto& evcb = it->second;
            uint32_t flags = evs[i].events;

            if (flags & EPOLLERR) {
                if (evcb.errCb_) evcb.errCb_();
                continue;
            }
            if (flags & EPOLLIN && evcb.readCb_) {
                evcb.readCb_();
            }
            if (flags & EPOLLOUT) {
                auto wit = this->event_map_.find(fd);
                if (wit != this->event_map_.end() && wit->second.writeCb_) {
                    wit->second.writeCb_();
                }
            }
        }

        this->do_pending_functors();
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

// 跨线程投递回调的关键函数
//
// 如果调用方已经在本事件循环线程中，直接执行回调
// 这样可以避免不必要的锁操作和 eventfd 唤醒
//
// 如果调用方是其他线程，把回调加入 pending_functors_ 队列，
// 然后通过 write eventfd 唤醒 epoll_wait
// 事件循环线程会在每轮循环末尾取出所有待办回调并执行
void EventLoop::run_in_loop(std::function<void()> cb) {
    if (this->is_in_loop_thread()) {
        cb();
    } else {
        {
            std::lock_guard<std::mutex> lock(this->mtx_functors_);
            this->pending_functors_.push_back(std::move(cb));
        }
        this->wakeup();
    }
}

// 写入 eventfd 来唤醒 epoll_wait
// eventfd 被注册在 epoll 的可读事件中，
// 写入后 epoll_wait 会立刻返回
void EventLoop::wakeup() {
    uint64_t x = 1;
    if (write(this->eventfd_, &x, sizeof(x)) < 0 && errno != EAGAIN) {
        perror("write eventfd");
    }
}

// 判断当前线程是否就是事件循环所在的线程
bool EventLoop::is_in_loop_thread() const {
    return this->loop_tid_ == std::this_thread::get_id();
}

// 返回 pending_functors_ 队列中待办回调的数量
// 这个值用于 MainReactor 做负载感知分发
// 数量越少说明这个 EventLoop 当前越空闲
size_t EventLoop::pending_size() const {
    return this->pending_functors_.size();
}

// 读取 eventfd 的数据
// eventfd 被唤醒后如果不读取数据，epoll 会一直报告可读
// 这里只是清空 eventfd 的状态，不做其他操作
void EventLoop::handle_eventfd() {
    uint64_t x;
    if (read(this->eventfd_, &x, sizeof(x)) < 0 && errno != EAGAIN) {
        perror("read eventfd");
    }
}

// 执行所有跨线程投递的待办回调
// 先加锁把 pending_functors_ 的数据交换到局部变量中，
// 然后尽快释放锁，最后逐个执行回调
// 这样可以减少锁的持有时间
void EventLoop::do_pending_functors() {
    std::vector<std::function<void()>> functors;
    {
        std::lock_guard<std::mutex> lock(this->mtx_functors_);
        functors.swap(this->pending_functors_);
    }
    for (auto& f : functors) {
        f();
    }
}
