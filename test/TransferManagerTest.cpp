// TransferManager 用例 — 文件注册 窗口启动 分块转发 ACK 推进
#include "TestMain.h"

#include <cstdint>
#include <memory>
#include <string>

#include "chatroom/TransferManager.h"
#include "ctrl/Session.h"

static constexpr size_t kChunk = TransferSession::kChunkSize;
static constexpr size_t kWindow = TransferSession::kWindowSize;

// 造一个 20 字节分块头加载荷 头部按小端落字节 与 TransferManager 的 memcpy 读法一致
static std::string make_chunk(uint64_t session_id, uint64_t offset, const std::string& payload) {
    uint32_t size = static_cast<uint32_t>(payload.size());
    std::string d;
    d.append(reinterpret_cast<const char*>(&session_id), 8);
    d.append(reinterpret_cast<const char*>(&offset), 8);
    d.append(reinterpret_cast<const char*>(&size), 4);
    d += payload;
    return d;
}

TEST(transfer_manager_registers_file) {
    TransferManager tm;
    auto uploader = std::make_shared<Session>(1, 0);
    std::string id = tm.register_file("a.bin", 4 * kChunk, uploader);
    CHECK(!id.empty());

    auto reg = tm.find_registration(id);
    CHECK(reg.has_value());
    CHECK_EQ(reg->filename_, std::string("a.bin"));
    CHECK_EQ(reg->filesize_, size_t(4 * kChunk));
    CHECK_EQ(reg->uploader_.get(), uploader.get());
}

TEST(transfer_manager_rejects_zero_byte_file) {
    TransferManager tm;
    auto uploader = std::make_shared<Session>(1, 0);
    CHECK(tm.register_file("empty.bin", 0, uploader).empty());
}

TEST(transfer_manager_reports_missing_registration) {
    TransferManager tm;
    CHECK(!tm.find_registration("no-such-id").has_value());
}

TEST(transfer_manager_starts_with_full_window) {
    TransferManager tm;
    auto uploader = std::make_shared<Session>(1, 0);
    auto downloader = std::make_shared<Session>(2, 0);
    std::string id = tm.register_file("a.bin", 100 * kChunk, uploader);

    TransferStart start = tm.start_transfer(id, downloader, 0);
    CHECK(start.valid_);
    CHECK_EQ(start.requests_.size(), kWindow);
    // 窗口逐块推进 每块偏移相差一个分块
    for (size_t i = 0; i < start.requests_.size(); ++i) {
        CHECK_EQ(start.requests_[i].offset_, i * kChunk);
        CHECK_EQ(start.requests_[i].size_, kChunk);
        CHECK_EQ(start.requests_[i].uploader_.get(), uploader.get());
    }
}

TEST(transfer_manager_shortens_last_chunk) {
    // 末块不足一个分块 请求长度按剩余字节算
    TransferManager tm;
    auto uploader = std::make_shared<Session>(1, 0);
    auto downloader = std::make_shared<Session>(2, 0);
    std::string id = tm.register_file("a.bin", kChunk + 7, uploader);

    TransferStart start = tm.start_transfer(id, downloader, 0);
    CHECK(start.valid_);
    CHECK_EQ(start.requests_.size(), size_t(2));
    CHECK_EQ(start.requests_[0].size_, kChunk);
    CHECK_EQ(start.requests_[1].size_, size_t(7));
}

TEST(transfer_manager_rejects_start_offset_past_end) {
    TransferManager tm;
    auto uploader = std::make_shared<Session>(1, 0);
    auto downloader = std::make_shared<Session>(2, 0);
    std::string id = tm.register_file("a.bin", 4 * kChunk, uploader);

    CHECK(!tm.start_transfer(id, downloader, 4 * kChunk).valid_);
}

TEST(transfer_manager_rejects_transfer_from_dead_uploader) {
    TransferManager tm;
    auto uploader = std::make_shared<Session>(1, 0);
    auto downloader = std::make_shared<Session>(2, 0);
    std::string id = tm.register_file("a.bin", 4 * kChunk, uploader);
    uploader->alive_ = false;

    CHECK(!tm.start_transfer(id, downloader, 0).valid_);
}

// 起一次传输 返回会话 id
static uint64_t begin_transfer(TransferManager& tm, const std::string& file_id,
                               const std::shared_ptr<Session>& downloader) {
    TransferStart start = tm.start_transfer(file_id, downloader, 0);
    CHECK(start.valid_);
    return start.session_id_;
}

TEST(transfer_manager_forwards_chunk_to_downloader) {
    TransferManager tm;
    auto uploader = std::make_shared<Session>(1, 0);
    auto downloader = std::make_shared<Session>(2, 0);
    std::string id = tm.register_file("a.bin", 100 * kChunk, uploader);
    uint64_t sid = begin_transfer(tm, id, downloader);

    std::string payload(kChunk, 'x');
    ChunkResult r = tm.handle_chunk_data(uploader.get(), make_chunk(sid, 0, payload));
    CHECK(r.valid_);
    CHECK_EQ(r.session_id_, sid);
    CHECK_EQ(r.offset_, size_t(0));
    CHECK_EQ(r.size_, kChunk);
    CHECK_EQ(r.downloader_.get(), downloader.get());
}

TEST(transfer_manager_rejects_chunk_from_other_session) {
    // 只有本会话的上传方有资格投递数据
    TransferManager tm;
    auto uploader = std::make_shared<Session>(1, 0);
    auto downloader = std::make_shared<Session>(2, 0);
    auto intruder = std::make_shared<Session>(3, 0);
    std::string id = tm.register_file("a.bin", 100 * kChunk, uploader);
    uint64_t sid = begin_transfer(tm, id, downloader);

    std::string payload(kChunk, 'x');
    CHECK(!tm.handle_chunk_data(intruder.get(), make_chunk(sid, 0, payload)).valid_);
}

TEST(transfer_manager_rejects_chunk_with_wrong_size) {
    // 声明长度与实际载荷必须一致
    TransferManager tm;
    auto uploader = std::make_shared<Session>(1, 0);
    auto downloader = std::make_shared<Session>(2, 0);
    std::string id = tm.register_file("a.bin", 100 * kChunk, uploader);
    uint64_t sid = begin_transfer(tm, id, downloader);

    CHECK(!tm.handle_chunk_data(uploader.get(), make_chunk(sid, 0, std::string(10, 'x'))).valid_);
}

TEST(transfer_manager_advances_window_on_each_step) {
    TransferManager tm;
    auto uploader = std::make_shared<Session>(1, 0);
    auto downloader = std::make_shared<Session>(2, 0);
    std::string id = tm.register_file("a.bin", 100 * kChunk, uploader);
    uint64_t sid = begin_transfer(tm, id, downloader);

    // 初始窗口是 0..kWindow-1 收下第 0 块便补发第 kWindow 块
    std::string payload(kChunk, 'x');
    ChunkResult cr = tm.handle_chunk_data(uploader.get(), make_chunk(sid, 0, payload));
    CHECK(cr.valid_);
    CHECK(cr.next_.has_value());
    CHECK_EQ(cr.next_->offset_, kWindow * kChunk);

    // 确认第 0 块后再补发第 kWindow+1 块
    AckResult ar = tm.handle_ack(downloader.get(), sid, 0);
    CHECK(ar.valid_);
    CHECK(ar.next_.has_value());
    CHECK_EQ(ar.next_->offset_, (kWindow + 1) * kChunk);
    CHECK_EQ(ar.next_->uploader_.get(), uploader.get());
}

TEST(transfer_manager_rejects_ack_from_other_session) {
    // 只有本会话的下载方有资格确认
    TransferManager tm;
    auto uploader = std::make_shared<Session>(1, 0);
    auto downloader = std::make_shared<Session>(2, 0);
    auto intruder = std::make_shared<Session>(3, 0);
    std::string id = tm.register_file("a.bin", 100 * kChunk, uploader);
    uint64_t sid = begin_transfer(tm, id, downloader);

    std::string payload(kChunk, 'x');
    tm.handle_chunk_data(uploader.get(), make_chunk(sid, 0, payload));
    CHECK(!tm.handle_ack(intruder.get(), sid, 0).valid_);
}

TEST(transfer_manager_rejects_ack_for_unrequested_offset) {
    // 该偏移不在待确认窗口内 属非法确认
    TransferManager tm;
    auto uploader = std::make_shared<Session>(1, 0);
    auto downloader = std::make_shared<Session>(2, 0);
    std::string id = tm.register_file("a.bin", 100 * kChunk, uploader);
    uint64_t sid = begin_transfer(tm, id, downloader);

    CHECK(!tm.handle_ack(downloader.get(), sid, 7 * kChunk).valid_);
}

TEST(transfer_manager_cancels_session_by_connection) {
    // 上传方断开时该连接参与的全部会话都要作废
    TransferManager tm;
    auto uploader = std::make_shared<Session>(1, 0);
    auto downloader = std::make_shared<Session>(2, 0);
    std::string id = tm.register_file("a.bin", 100 * kChunk, uploader);
    uint64_t sid = begin_transfer(tm, id, downloader);

    CancelResult cr = tm.cancel_by_session(uploader);
    CHECK_EQ(cr.cancelled_.size(), size_t(1));
    CHECK_EQ(cr.cancelled_[0].file_id_, id);
    CHECK_EQ(cr.cancelled_[0].orphaned_downloader_.get(), downloader.get());
    // 会话已作废 对其确认不再有效
    CHECK(!tm.handle_ack(downloader.get(), sid, 0).valid_);
}
