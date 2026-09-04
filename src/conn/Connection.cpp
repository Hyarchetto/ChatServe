// Connection::send — 发送入口实现
// 归属写引擎内含归属事件循环 本地判定与跨线程投递都以该循环为目标
// 当前线程即归属线程时直接入队排空 否则投到归属线程执行
#include "conn/Connection.h"
#include "conn/WriteScheduler.h"
#include "core/EventLoop.h"

#include <utility>

void Connection::send(const std::shared_ptr<Connection>& conn,
                      std::string data, bool is_high_priority) {
    if (!conn->alive_) {
        return;
    }
    WriteScheduler* writer = &conn->writer_;
    EventLoop& target = writer->loop();
    // 归属线程直投 其它线程经 run_in_loop 切到归属线程入队排空
    if (target.is_in_loop_thread()) {
        writer->enqueue(conn, std::move(data), is_high_priority);
    }
    else {
        target.run_in_loop([writer, conn, data = std::move(data),
                            is_high_priority]() mutable {
            writer->enqueue(std::move(conn), std::move(data), is_high_priority);
        });
    }
}
