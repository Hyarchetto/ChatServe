// 单元测试入口 — 顺序跑完全部注册用例 汇总结果
#include "TestMain.h"

int main() {
    for (auto& tc : test_cases()) {
        test_failed() = 0;
        ++test_total_cases();
        tc.fn_();
        if (test_failed() > 0) {
            ++test_failed_cases();
            std::printf("失败 %s（%d 处断言）\n", tc.name_, test_failed());
        }
        else {
            std::printf("通过 %s\n", tc.name_);
        }
    }

    std::printf("\n用例 %d 个，失败 %d 个\n",
                test_total_cases(), test_failed_cases());
    return test_failed_cases() == 0 ? 0 : 1;
}
