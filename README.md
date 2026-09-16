# ChatServe

一个从 epoll 手写起的多人在线聊天服务器。

**不依赖任何第三方库** —— 事件循环、HTTP / WebSocket 协议解析、线程调度、文件传输全部自建，连 WebSocket 握手所需的 SHA1 也是自己实现的。从 `epoll_wait` 到帧解析、从线程归属到传输窗口，每一层都在这个仓库里。

配套 Vue 3 前端与 Electron 桌面外壳。C++17，服务端约 4500 行，前端约 1500 行，单元测试 66 个用例。

## 功能

| 功能 | 说明 |
| --- | --- |
| 实时聊天 | 房间制，成员进出广播，消息走 WebSocket 全双工 |
| 文件传输 | 应用层滑动窗口 + 逐块 ACK + 断点续传 |
| WebRTC 音视频 | 服务器只中转信令，媒体流走浏览器 P2P 直连 |
| HTTP 服务 | 前端页面与静态资源，含路径穿越防护 |
| 桌面客户端 | Electron 外壳，可打包为单文件 exe |

## 整体架构

### 分层与依赖

依赖严格单向：

```
core          事件循环、线程池                根，无内部依赖
  ↑
ctrl          Session、跨线程消息、邮箱
  ↑
chatroom      房间与房间内的文件传输
  ↑
app           应用层命令路由

http          HTTP 解析、路由、静态文件       无内部依赖，与 core 并列
  ↑
ws            WebSocket 帧层与握手，依赖 http

conn          连接泵：读缓冲、分帧、施加决策
              依赖 core / ctrl / http / ws
  ↑
server        装配层：Reactor、网关、中控、工厂
              依赖其余全部
```

**全树没有反向边，也没有前向声明** —— 头文件一律直接 include 所需类型，不靠传递包含。

### 线程模型

```
                    ┌──────────────┐
   accept ─────────▶│  主 Reactor  │  fd % N 哈希分发
                    └──────┬───────┘
          ┌────────────────┼────────────────┐
          ▼                ▼                ▼
   ┌────────────┐   ┌────────────┐   ┌────────────┐
   │ io 从属 0  │   │ io 从属 1  │   │ io 从属 N  │   各一条线程
   │ Connection │   │ Connection │   │ Connection │
   └─────┬──────┘   └─────┬──────┘   └─────┬──────┘
         │  CtrlUp        │  CtrlUp        │  CtrlUp
         └────────────────┼────────────────┘
                          ▼
                  ┌───────────────┐
                  │   中控线程    │  唯一分发点 + 业务串行门
                  └───────┬───────┘
                          ▼
                  ┌───────────────┐
                  │  业务线程池   │  AppRouter 命令表
                  └───────────────┘
```

四个执行体，职责边界清晰：

- **主线程** — 只跑主 Reactor 的监听与分发。accept 到 fd 后按 `fd % N` 交给某个 io 线程，自己不碰连接
- **io 线程** — 只管 socket 生命周期。读、解析、组帧、写。业务不上本线程
- **中控线程** — io 与业务之间的解耦层。收上行事件、维持单飞门、把下行帧按归属 io 攒批投递
- **业务线程池** — 纯计算。只按 `Session` 控制块算响应，不碰会话容器也不碰 io 邮箱

### 一条消息的完整路径

```
io 线程    recv → read_buf_ → 协议解析 → 组 CtrlUp
              ↓ Mailbox（跨线程单消费者邮箱，合并唤醒）
中控线程    handle_uplink → 单飞门判定 → submit 进线程池
              ↓
业务线程池  AppRouter::handle → 命中命令表 → handler 算出待发帧
              ↓ EventLoop::post
中控线程    on_done → dispatch 按 Session::io_ 分组 → 投进对应 io 的下行邮箱
              ↓ Mailbox
io 线程    downlink → 组 WS 帧 → WriteScheduler 排空
```

## 关键设计

**跨线程身份用 `shared_ptr` 保活，不用 fd。** `Session` 是横跨 io / 中控 / 业务三方的共享控制块，靠引用计数保证任务在队列里排队期间不被释放。用 fd 会在连接断开后重用时误命中，用 `weak_ptr` 则任务执行时读不到业务态（房间号、昵称），而断连清理恰恰需要它们。`Session` 的裸指针只出现在中控线程私有的索引里（`pending_` / `running_` 的 key），且由同一批 `shared_ptr` 担保存活。

**`Connection` 与 `Session` 的分界就是线程边界。** `Connection` 是 io 线程私有状态（socket RAII、读缓冲、WS 分片累积），绝不跨线程；`Session` 跨线程。这条边界由类型而非注释保证。

**单飞门保证同一连接的命令顺序。** 同一 `Session` 至多一条命令在业务池中；后续命令入队而不是并发提交。业务完成后由中控线程拉下一条。这样业务层不需要为「同一连接的两条消息谁先改房间状态」加锁。

**下行帧存活的唯一判据是 `dispatch`。** 业务层不判断「这个帧的目标还在不在」，一律交给中控的 `dispatch` 统一按 `alive_` 弃帧。业务层读 `alive_` 只在**读完会产生业务动作**时才合法 —— 比如传输层发现对端已断线要主动回一条错误帧通知另一方；静默弃帧在文件传输里等于让对方永久挂起。

**ET 模式下的写路径。** `EPOLLET` 要求把缓冲区读空，写路径同理：没发完的字节进待写缓冲并注册 `EPOLLOUT`，且立即尝试冲刷一次防饥饿。慢客户端会让缓冲持续增长，因此在追加前判上限，超限直接断开而不是无限缓存。

## 目录结构

```
include/  src/           C++ 服务端（按上面各层分目录）
web/                     Vue 3 前端源码
static/                  web/ 构建产物，服务器直接托管
client/                  Electron 桌面外壳
server/main.cpp          进程入口
test/                    单元测试（手写断言，零第三方依赖）
scripts/
  run.sh                 一键启动（构建 + 服务 + HTTPS 代理）
  https_proxy.py         自签证书 TLS 透传，8443 → 8080
build.sh                 构建脚本
```

## 构建与运行

需要 CMake ≥ 3.14 与支持 C++17 的编译器。**一条命令跑起来**：

```bash
./scripts/run.sh          # 构建 + 起服务，证书齐全时顺带拉起 HTTPS 代理
```

浏览器打开 `http://localhost:8080/chat`，开两个标签页各填一个昵称、进同一个房间，就能看到聊天与成员广播；点输入框左侧的回形针发文件。视频通话需要 HTTPS，见下。默认监听 8080，io worker 数量在 `server/main.cpp` 的 `kIoCount` 调整。

不用脚本时，等价的两步是 `./build.sh` 构建、`./build/server/chat_server` 起服务。

**单元测试**（66 个用例，覆盖协议解析、缓冲、传输窗口）：

```bash
./build/test/chat_tests     # 直接跑，逐条打印结果
cd build && ctest           # 或走 CTest
```

**HTTPS 代理**（WebRTC 的摄像头麦克风只在安全上下文可用，所以需要 TLS）：

```bash
python3 scripts/https_proxy.py      # 8443 → 8080
```

浏览器访问 `https://<本机 IP>:8443/chat`，首次需要手动信任自签证书。

仓库只带了 `scripts/cert.pem`，**私钥不随仓库分发**（`run.sh` 检测到缺私钥会跳过代理）。自己签一对：

```bash
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
  -keyout scripts/key.pem -out scripts/cert.pem -subj "/CN=chatserve"
```

**前端**：

```bash
cd web && npm install && npm run build   # 产物输出到 ../static
```

**桌面客户端**：

```bash
cd client && npm install && npm run dist  # 产出 dist/ChatServe.exe
```

服务器地址可配置，优先级：`--server=<地址>` 参数 → `CHATSERVE_SERVER` 环境变量 → 内置默认。

## 通信协议

应用层是自定义文本协议，`|` 分隔：`COMMAND|param1|param2|...`

**客户端 → 服务端**

| 域 | 命令 |
| --- | --- |
| 房间 | `JOIN` |
| 聊天 | `MSG` |
| 信令 | `OFFER` `ANSWER` `ICE` `MEDIA` |
| 传输 | `UPLOAD` `UPCANCEL` `DOWNLOAD` `DWACK` `DWNPAUSE` `DWNCANCEL` |

**服务端 → 客户端**

| 域 | 帧 |
| --- | --- |
| 房间 | `OK` `SYS` `MEMBERS` `JOIN` `LEAVE` |
| 聊天 | `MSG` |
| 信令 | `OFFER` `ANSWER` `ICE` `MEDIA` |
| 传输 | `UPOK` `FILE` `DONE` `DWSTART` `DWREQ` `DWERR` `DWDATA` `DWNDONE` |

无 `|` 的裸文本与未注册命令一律丢弃。

**文件分块**走 WS 二进制帧，载荷前 20 字节是固定头，其余为文件数据：

```
[session_id:8 LE][offset:8 LE][data_size:4 LE][data...]
```

**传输流程**：下载方向上传方发起 `DWREQ` → 上传方按请求回分块 → 服务器转发给下载方 → 下载方 `DWACK` 确认 → 服务器补发下一个请求。窗口 8 块 × 256KB，`start_offset` 支持断点续传。

## 性能

回环网络下的文件传输实测，每房间一上传方一下载方，客户端用多进程绕开 Python GIL：

| 场景 | 总量 | 耗时 | 聚合吞吐 |
| --- | --- | --- | --- |
| 1 房间 × 256MB（单连接基线） | 256 MB | 0.32 s | 804 MB/s |
| 8 房间 × 64MB | 512 MB | 0.26 s | 1976 MB/s |
| 16 房间 × 64MB | 1 GB | 0.47 s | 2188 MB/s |
| 32 房间 × 64MB | 2 GB | 0.91 s | 2255 MB/s |

全部传输成功、零失败，四个 io 从属 Reactor 的连接负载完全均衡（`fd % 4` 各 16）。

同一组 16 房间的对照：客户端改成单进程线程模式只有 **397 MB/s**，差五倍多。这说明该数字受限于**测试客户端的 GIL**，不是服务器的上限。

**这些数字要打折扣看**：测的是回环（内存拷贝）而非真实网卡，客户端侧也仍可能是瓶颈，所以 2.2 GB/s 是下限不是上限。

## 已知限制

当前实现未覆盖的部分，也是后续更新迭代方向：

- **无认证**。知道房间名即可加入，用户名可任意伪造
- **无 TLS**。明文 `ws://` / `http://`。WebRTC 要求安全上下文，本地演示靠 `scripts/https_proxy.py` 在服务外终结 TLS
- **无连接数上限，也无心跳与空闲超时**。不发数据的连接会一直占着 fd 与内存；服务端能正确回应客户端的 PING，但不主动探测半开连接
- **无背压**。单连接待处理的命令队列不设上限，持续刷屏会堆积内存
- **无持久化，单机**。消息不落库，重启即丢；房间不跨进程或跨机
- **大帧会阻塞同连接的后续帧**。WS 不允许帧交错，二进制分块发到一半时后到的文本帧只能排在它之后，发送队列是单一 FIFO，不做优先级调度
- **测试只覆盖纯逻辑**。连接建立、线程交互、WebRTC 信令这些跨单元的部分没有自动化测试，靠手工验证
