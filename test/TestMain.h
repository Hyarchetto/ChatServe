// 极简单元测试框架 — 零第三方依赖
// 用例用 TEST 宏声明 注册到全局表 由 TestMain.cpp 的 main 顺序执行
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

// 用例注册表与计数 头文件里用 inline 变量避免多 TU 重复定义
struct TestCase {
    const char* name_;
    std::function<void()> fn_;
};

inline std::vector<TestCase>& test_cases() {
    static std::vector<TestCase> cases;
    return cases;
}

// 一个用例内部失败的断言数 用例开始时清零
inline int& test_failed() {
    static int failed = 0;
    return failed;
}

// 全局统计
inline int& test_total_cases() {
    static int total = 0;
    return total;
}

inline int& test_failed_cases() {
    static int failed = 0;
    return failed;
}

// 静态注册器 声明用例时构造一次
struct TestReg {
    TestReg(const char* name, std::function<void()> fn) {
        test_cases().push_back(TestCase{name, std::move(fn)});
    }
};

// 断言失败时打印值 算术打十进制 指针打地址 其余按字符串打
template <typename T>
void test_print_value(const char* label, const T& v) {
    if constexpr (std::is_arithmetic_v<T>) {
        std::printf("%s%lld\n", label, static_cast<long long>(v));
    }
    else if constexpr (std::is_pointer_v<T>) {
        std::printf("%s%p\n", label, static_cast<const void*>(v));
    }
    else {
        std::printf("%s%s\n", label, std::string(v).c_str());
    }
}

#define TEST(name)                                              \
    static void name();                                         \
    static TestReg test_reg_##name(#name, name);                \
    static void name()

// 条件断言 失败计数并打印表达式
#define CHECK(cond)                                             \
    do {                                                        \
        if (!(cond)) {                                          \
            ++test_failed();                                    \
            std::printf("  断言失败 %s:%d  %s\n",               \
                        __FILE__, __LINE__, #cond);             \
        }                                                       \
    } while (0)

// 相等断言 失败时打印两侧实际值
#define CHECK_EQ(a, b)                                          \
    do {                                                        \
        auto&& test_lhs_ = (a);                                 \
        auto&& test_rhs_ = (b);                                 \
        if (!(test_lhs_ == test_rhs_)) {                        \
            ++test_failed();                                    \
            std::printf("  断言失败 %s:%d  %s == %s\n",         \
                        __FILE__, __LINE__, #a, #b);            \
            test_print_value("    左 = ", test_lhs_);           \
            test_print_value("    右 = ", test_rhs_);           \
        }                                                       \
    } while (0)
