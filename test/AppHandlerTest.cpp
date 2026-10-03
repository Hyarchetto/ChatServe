// AppHandler 用例 — 命令投池后经结果邮箱回中控的两条分支
#include "TestMain.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/AppHandler.h"
#include "core/EventLoop.h"
#include "core/Mailbox.h"
#include "core/ThreadPool.h"
#include "ctrl/CtrlMsg.h"
#include "ctrl/Session.h"

namespace {

// 业务池 + 中控结果邮箱 + 跑在真实线程上的事件循环，凑齐 AppHandler 的运行条件
// 结果在邮箱回调里攒下，用例据此断言语义
struct Fixture {
    EventLoop loop_;
    ThreadPool pool_{1};
    AppHandler app_{pool_};
    Mailbox<CtrlResult> result_box_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::vector<CtrlResult> got_;
    std::thread runner_;

    Fixture()
        : result_box_([this](std::vector<CtrlResult>& results) {
              std::lock_guard<std::mutex> lock(this->mtx_);
              for (auto& r : results) {
                  this->got_.push_back(std::move(r));
              }
              this->cv_.notify_all();
          }) {
        CHECK(this->loop_.init());
        CHECK(this->result_box_.attach(this->loop_));
        this->app_.attach_result_box(this->result_box_);
        this->runner_ = std::thread([this]() { this->loop_.loop(); });
    }

    // 先收池再收循环，在途任务向邮箱投递完才轮到循环退出
    ~Fixture() {
        this->pool_.shutdown();
        this->loop_.quit();
        this->runner_.join();
    }

    // 等到至少 n 条结果，超时即失败，判错时不把用例挂住
    bool wait_results(size_t n) {
        std::unique_lock<std::mutex> lock(this->mtx_);
        return this->cv_.wait_for(lock, std::chrono::seconds(2),
                                  [&]() { return this->got_.size() >= n; });
    }
};

}  // namespace

TEST(app_handler_runs_text_command_on_pool_and_posts_back) {
    Fixture f;
    auto sess = std::make_shared<Session>(7, 0);

    f.app_.handle({CtrlCmd{sess, CtrlUpKind::WS_TEXT, "PING|"}});

    CHECK(f.wait_results(1));
    std::lock_guard<std::mutex> lock(f.mtx_);
    CHECK_EQ(f.got_.size(), size_t(1));
    CHECK(f.got_[0].sess_.get() == sess.get());  // 寻址回发起会话
    CHECK_EQ(f.got_[0].frames_.size(), size_t(1));
    CHECK_EQ(f.got_[0].frames_[0].text_, std::string("PONG|"));
    CHECK(f.got_[0].frames_[0].kind_ == CtrlDownKind::WS_TEXT);
}

TEST(app_handler_cleans_up_on_close_without_frames) {
    Fixture f;
    auto sess = std::make_shared<Session>(9, 0);
    CHECK(sess->room_.empty());

    f.app_.handle({CtrlCmd{sess, CtrlUpKind::CLOSED, ""}});

    // 未进房的会话收尾自然无帧，但结果要照样带回中控，车道才推进得下去
    CHECK(f.wait_results(1));
    std::lock_guard<std::mutex> lock(f.mtx_);
    CHECK_EQ(f.got_.size(), size_t(1));
    CHECK(f.got_[0].sess_.get() == sess.get());
    CHECK_EQ(f.got_[0].frames_.size(), size_t(0));
}

TEST(app_handler_hands_back_whole_batch_when_pool_stopped) {
    Fixture f;
    auto sess = std::make_shared<Session>(3, 0);
    f.pool_.shutdown();  // 池已停，投递必失败

    std::vector<Session*> failed =
        f.app_.handle({CtrlCmd{sess, CtrlUpKind::WS_TEXT, "PING|"}});

    // 全有或全无，一条都没进池就整批退回，调用方据此撤车道
    CHECK_EQ(failed.size(), size_t(1));
    CHECK(failed[0] == sess.get());
}

TEST(app_handler_posts_one_result_per_command_in_batch) {
    Fixture f;
    auto first = std::make_shared<Session>(1, 0);
    auto second = std::make_shared<Session>(2, 0);

    // 一轮投两条，池线程各跑一条，回程是两条独立结果
    f.app_.handle({CtrlCmd{first, CtrlUpKind::WS_TEXT, "PING|"},
                   CtrlCmd{second, CtrlUpKind::WS_TEXT, "PING|"}});

    CHECK(f.wait_results(2));
    std::lock_guard<std::mutex> lock(f.mtx_);
    CHECK_EQ(f.got_.size(), size_t(2));
    for (const CtrlResult& r : f.got_) {
        CHECK_EQ(r.frames_.size(), size_t(1));
        CHECK_EQ(r.frames_[0].text_, std::string("PONG|"));
    }
}
