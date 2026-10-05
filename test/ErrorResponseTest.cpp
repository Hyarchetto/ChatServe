// ErrorResponse 用例 — 错误种类到状态码与页面的映射
#include "TestMain.h"

#include <string>

#include "http/ErrorResponse.h"

TEST(error_response_gives_each_kind_its_status) {
    // 头超限与体超限各有语义码，其余 4xx 落在 400，读文件失败是 500
    CHECK_EQ(ErrorResponse::build(HttpError::HEADER_TOO_LARGE).status_, 431);
    CHECK_EQ(ErrorResponse::build(HttpError::BODY_TOO_LARGE).status_, 413);
    CHECK_EQ(ErrorResponse::build(HttpError::FILE_TOO_LARGE).status_, 413);
    CHECK_EQ(ErrorResponse::build(HttpError::READ_FAILED).status_, 500);
    CHECK_EQ(ErrorResponse::build(HttpError::DUPLICATE_LENGTH).status_, 400);
    CHECK_EQ(ErrorResponse::build(HttpError::MISSING_WS_KEY).status_, 400);
}

TEST(error_response_puts_the_reason_in_the_page) {
    // 状态行留协议的原因短语，页面上给人看的是中文说明
    HttpResponse r = ErrorResponse::build(HttpError::HEADER_TOO_LARGE);
    CHECK(r.body_.find("431 Request Header Fields Too Large") != std::string::npos);
    CHECK(r.body_.find("请求头超过上限") != std::string::npos);
}

TEST(error_response_escapes_the_path_in_the_page) {
    // 路径进 HTML 前要转义，文件名里的尖括号不能原样落进页面
    HttpResponse r = ErrorResponse::build_not_found("a<b>.png");
    CHECK_EQ(r.status_, 404);
    CHECK(r.body_.find("a&lt;b&gt;.png") != std::string::npos);
}
