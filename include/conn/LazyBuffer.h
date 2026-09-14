// 懒头 FIFO 缓冲 尾部追加 头部消费只推进 head_ 攒够阈值才物理压缩
#pragma once

#include <string>
#include <algorithm>

class LazyBuffer {
public:
    // 可读连续区 先查 size() 再访问
    const char* data() const { 
        return data_.data() + head_; 
    }
    size_t size() const { 
        return data_.size() - head_; 
    }

    bool empty() const { 
        return head_ >= data_.size(); 
    }

    void append(const char* p, size_t n) {
        data_.append(p, n); 
    }
    void append(const std::string& s) { 
        data_.append(s); 
    }
    void append(std::string&& s) { 
        data_.append(std::move(s)); 
    }

    // 消费 n 字节 
    void consume(size_t n) {
        head_ = std::min(head_ + n, data_.size());
        //  数据全消费完直接重置
        if (head_ >= data_.size()) {
            this->clear();
            return;
        }
        // 当已消费数据超过4KB并且占全部数据大半的内存占用时清除
        if (head_ > kCompactThreshold && head_ >= data_.size() / 2) {
            data_.erase(0, head_);
            head_ = 0;
        }
    }

    void clear() {
        data_.clear();
        head_ = 0;
    }

private:
    static constexpr size_t kCompactThreshold = 4096;   // 压缩下界 4KB
    std::string data_;
    size_t head_ = 0;
};
