// FdRegistration — 一条 fd 在事件循环里的注册
// 对象活着即该 fd 已注册 析构即摘除并关闭
// 只管接管 注册 摘除 建 fd 由持有者自己管
// 只服务唤醒类 fd 监听标志固定为 EPOLLIN 电平触发 8 字节计数的读写也归这里
//
// 契约 注册须在 loop.init() 之后 loop.loop() 之前 事件表非线程安全
//      析构须在 loop 线程停下之后 那时才没人再碰事件表
#pragma once

#include <functional>

class EventLoop;   // 循环引用 本类要注册进 EventLoop 而 EventLoop 自持一份唤醒注册

class FdRegistration {
public:
    FdRegistration() = default;
    ~FdRegistration();

    FdRegistration(const FdRegistration&) = delete;
    FdRegistration& operator=(const FdRegistration&) = delete;

    // 接管一个已建好的 fd 并注册它 到可读时回调 on_ready
    bool attach(EventLoop& loop, int fd, std::function<void()> on_ready);

    // 提前摘除并关闭 供持有者与其它资源定序 此后本对象回到未接管状态
    void detach();

    // 计数加一唤醒消费线程 未接管时无处可写 留待挂上后的第一次唤醒一并取走
    // timerfd 不可写 只有 eventfd 用得上
    void wakeup();

    // 读干计数 未接管时静默
    // 漏读一次 fd 就一直可读 电平触发下 epoll_wait 空转
    void drain();

    // 已接管返回 fd 未接管返回 -1
    int fd() const { return this->fd_; }

private:
    EventLoop* loop_ = nullptr;   // 析构摘除使用
    int fd_ = -1;
};
