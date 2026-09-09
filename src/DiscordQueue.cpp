#include "DiscordQueue.h"

void DiscordQueue::Push(DiscordOutMessage msg)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        messages_.push_back(std::move(msg));
    }
    cv_.notify_one();
}

bool DiscordQueue::Pop(DiscordOutMessage& out, uint32 timeoutMs)
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return !messages_.empty(); }))
    {
        out = std::move(messages_.front());
        messages_.pop_front();
        return true;
    }
    return false;
}