// 连接对象 — io 线程私有每连接状态的聚合 兼 socket 生命周期的 RAII 持有
// fd/alive 统一存于共享 Session 控制块 那是唯一事实来源 本对象不重复存
// 全成员只被归属 io 线程触碰 绝不跨线程 与跨线程的 Session 的分工即线程边界
// ws_mode_ 是本连接是否已升级 ws_frag_ 是 read_buf_ 的溢出段
// 完整帧已 consume 但消息未成形 载荷不能丢 只能挪出来存着 与读缓冲同处
// 写只经所在 io 线程的写引擎发生 本对象不携带身份与写引擎
#pragma once

#include <memory>
#include <cerrno>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

#include "LazyBuffer.h"
#include "../ctrl/Session.h"
#include "../ws/WsFragmentState.h"

class Connection {
public:
    std::shared_ptr<Session> sess_;     // 共享控制块 fd/alive/身份的唯一来源
    LazyBuffer read_buf_;               // 累积读取缓冲区
    bool ws_mode_ = false;              // 是否已升级为 WebSocket
    WsFragmentState ws_frag_;           // 未成形分片消息的累积 读缓冲的溢出段

    // 按 fd 与归属 io 在内部建 Session 控制块 连接在则 Session 有主 析构即连接终结
    // 设不上非阻塞就不进入连接生命周期，构造抛出由建连处收尾关 fd
    explicit Connection(int fd, int io)
        : sess_(std::make_shared<Session>(fd, io)) {
        this->set_nonblock();
    }

    ~Connection() {
        // socket 生命周期归 io 本对象析构即连接终结 关闭 fd
        ::close(this->sess_->fd_);
    }

private:
    // 设非阻塞失败必须让构造失败，半个非阻塞的 fd 会让整个 io 循环卡在 recv 上
    void set_nonblock() {
        int flags = fcntl(this->sess_->fd_, F_GETFL, 0);
        if (flags < 0 || fcntl(this->sess_->fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
            throw std::system_error(errno, std::generic_category(), "set O_NONBLOCK");
        }
    }
};
