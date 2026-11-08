// Hands the newest value from one thread to another without either ever waiting.
#pragma once

#include <array>
#include <atomic>

// Three slots, so the writer always has one free and the reader keeps the one it's reading.
template <typename T>
class Latest {
public:
    T& next() { return slots_[write_]; }
    void publish() { write_ = state_.exchange(write_ | kFresh) & kIndex; }
    bool take() {
        if (!(state_.load() & kFresh)) return false;
        read_ = state_.exchange(read_) & kIndex;
        return true;
    }
    T& latest() { return slots_[read_]; }
    std::array<T, 3>& slots() { return slots_; }

private:
    static constexpr int kIndex = 3, kFresh = 4;
    std::array<T, 3> slots_{};
    std::atomic<int> state_{1};
    int write_ = 0, read_ = 2;
};
