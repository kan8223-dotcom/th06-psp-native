#pragma once
// Shared by adapter and host abnormal-path tests; owns no allocations.
namespace th06midi {
struct BorrowedPayload { const void *data; unsigned int bytes; };
class MeBorrowedPayloads
{
  public:
    enum { kCapacity = 17 }; // current SMF + the command queue's 16 PLAY entries
    MeBorrowedPayloads() : count_(0), target_{nullptr, 0}, frozen_(false) {}
    bool canPlay(const void *data) const
    {
        if (frozen_) return false;
        for (unsigned int i = 0; i < count_; ++i)
            if (payloads_[i].data == data) return true;
        return count_ < kCapacity;
    }
    bool play(const void *data, unsigned int bytes)
    {
        if (!canPlay(data)) return false;
        target_ = BorrowedPayload{data, bytes};
        for (unsigned int i = 0; i < count_; ++i)
            if (payloads_[i].data == data) return true;
        payloads_[count_++] = target_;
        return true;
    }
    void stop() { if (!frozen_) target_ = BorrowedPayload{nullptr, 0}; }
    void applied()
    {
        if (frozen_) return;
        count_ = target_.data ? 1u : 0u;
        if (count_) payloads_[0] = target_;
    }
    void reinitialized()
    {
        if (frozen_) return;
        target_ = BorrowedPayload{nullptr, 0};
        count_ = 0;
    }
    void freeze() { frozen_ = true; }
    bool contains(const void *data) const
    {
        for (unsigned int i = 0; i < count_; ++i)
            if (payloads_[i].data == data) return true;
        return false;
    }
    unsigned int bytes() const
    {
        unsigned int n = 0;
        for (unsigned int i = 0; i < count_; ++i) n += payloads_[i].bytes;
        return n;
    }
  private:
    BorrowedPayload payloads_[kCapacity];
    unsigned int count_;
    BorrowedPayload target_;
    bool frozen_;
};
} // namespace th06midi
