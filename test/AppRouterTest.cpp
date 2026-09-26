// AppRouter 用例 — 心跳命令的回包与裸文本的丢弃
#include "TestMain.h"

#include <memory>
#include <string>
#include <vector>

#include "app/AppParser.h"
#include "app/AppRouter.h"
#include "chatroom/Room.h"
#include "ctrl/Session.h"

TEST(app_router_replies_pong_to_client_ping) {
    RoomManager rooms;
    AppRouter router(rooms);
    auto sess = std::make_shared<Session>(7, 0);

    // 客户端发的是带尾分隔符的 PING| 应答就是对这一条的 PONG|
    std::vector<CtrlDown> frames = router.handle(sess, AppParser::parse("PING|"));

    CHECK_EQ(frames.size(), size_t(1));
    CHECK(frames[0].sess_.get() == sess.get());  // 寻址回发起会话 无需新管道
    CHECK_EQ(frames[0].text_, std::string("PONG|"));
    CHECK(frames[0].kind_ == CtrlDownKind::WS_TEXT);
}

TEST(app_router_drops_bare_ping_without_delimiter) {
    // 无 | 落在解析器的裸文本分支 命令字为空 表查找必然未命中
    // 与上一条成对 客户端组帧一旦漏掉尾分隔符 表现就是静默无回包
    RoomManager rooms;
    AppRouter router(rooms);
    auto sess = std::make_shared<Session>(7, 0);

    CHECK_EQ(router.handle(sess, AppParser::parse("PING")).size(), size_t(0));
}

TEST(app_router_answers_ping_before_join) {
    // 心跳域不依赖房间 未进房也要能拿到回包 否则刚连上还没 JOIN 就被客户端判死
    RoomManager rooms;
    AppRouter router(rooms);
    auto sess = std::make_shared<Session>(7, 0);
    CHECK(sess->room_.empty());

    CHECK_EQ(router.handle(sess, AppParser::parse("PING|")).size(), size_t(1));
}
