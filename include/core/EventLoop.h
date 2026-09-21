// 事件循环
// 封装了 epoll 和跨线程任务投递机制，不包含任何业务
// 每个 EventLoop 独占一个线程，无限循环执行 epoll_wait
// 线程池干完活了就用 post 把结果塞回 IO 线程
// post 只投递到目标线程执行 不判当前线程 一律入队唤醒
#pragma once

#include <atomic>
#include <functional>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <cstdint>

class EventLoop {
public:
    // 构造时不做任何事情，真正的初始化在 init 中完成
    EventLoop();
    ~EventLoop();

    // ==================== 工作接口 ====================

    // 创建 epoll 句柄和 eventfd，把 eventfd 注册到 epoll 中
    bool init();

    // 事件循环主函数
    // 本函数不会返回，直到 quit 被调用
    void loop();

    // 设置退出标志并唤醒 epoll_wait 可在线程池或信号处理器中调用
    void quit();

    // ==================== epoll 事件注册 ====================

    // 把一个 fd 注册到 epoll 中
    // events 是 EPOLLIN EPOLLOUT EPOLLET 等标志的组合
    // read_cb  为读事件回调
    // write_cb 为写事件回调
    // err_cb   为错误事件回调
    // 注册失败返回 false，此时不保留回调，调用方据此关连接
    bool add_event(int fd, uint32_t events,
                   std::function<void()> read_cb = nullptr,
                   std::function<void()> write_cb = nullptr,
                   std::function<void()> err_cb = nullptr);

    // 从 epoll 中移除一个 fd，同时删除它关联的三个回调
    void del_event(int fd);

    // 修改一个 fd 在 epoll 中的监听事件
    // 失败返回 false，ET 下写事件没挂上去就是永久饥饿，调用方据此关连接
    bool mod_event(int fd, uint32_t events);

    // ==================== 跨线程任务投递 ====================

    // 把回调投递到本循环所属线程执行 跨线程安全
    // 只入队并唤醒 不保证立即执行 本 loop 线程自己调也一样入队
    // 本线程已有待办在途时只入队不再唤醒 重复唤醒由 draining_ 合并掉
    void post(std::function<void()> cb);

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

    // 等待队列，线程池投回来的待办回调都追加到这里，锁保护
    std::vector<std::function<void()>> pending_functors_;

    // 就绪队列，攒齐了正要执行的那批，与 pending_functors_ 互换复用容量
    // 只本 loop 线程碰，留着不销毁是免得每轮排空都重新分配
    std::vector<std::function<void()>> ready_functors_;

    // 有排空在途，与 pending_functors_ 同锁保护，投递方据此省掉重复的 eventfd 写
    bool draining_ = false;

    std::atomic<bool> quit_ = false;   // 退出标志，loop 函数每轮都会检查

    // 读取 eventfd 中的数据，清空唤醒标记
    void handle_eventfd();

    // 把 pending_functors_ 中的回调全部取出并执行
    void do_pending_functors();

    static constexpr int kMaxEvents = 1024;
};
