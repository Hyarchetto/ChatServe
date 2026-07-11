// 连接接收器
// 封装了监听套接字的创建、绑定、监听和非阻塞 accept 操作
// Acceptor 本身不持有一个 EventLoop，而是由外部传入一个
// 通常由 MainReactor 持有并使用
#pragma once

#include <functional>
#include <cstdint>

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

class EventLoop;

class Acceptor {
public:
    // 新连接到达时的回调类型
    // 参数是新 accept 出来的客户端文件描述符
    using NewConnectionCallback = std::function<void(int client_fd)>;

    Acceptor();
    ~Acceptor();

    // 创建监听套接字、绑定端口、开始监听，并注册到 event_loop 中
    // 监听套接字被设置为非阻塞模式并采用边缘触发
    // 返回 true 表示启动成功
    bool start(int port, EventLoop* loop);

    // 设置新连接到达时的回调函数
    // 这个回调会被传入新 accept 出来的客户端 fd
    void set_new_connection_callback(NewConnectionCallback cb);

private:
    int listenfd_ = -1;               // 监听套接字
    EventLoop* loop_ = nullptr;       // 所属的事件循环
    NewConnectionCallback new_conn_cb_;  // 新连接回调

    // 真正的 accept 处理函数
    // 采用 while 循环一次性收尽所有新连接
    // 每个新连接通过 new_conn_cb_ 回调交给上层处理
    void handle_accept();
};
