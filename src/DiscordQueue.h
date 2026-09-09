#ifndef DISCORD_QUEUE_H_
#define DISCORD_QUEUE_H_

#include "DiscordModule.h"
#include <condition_variable>
#include <deque>
#include <thread>

// Lock-guarded outbound message queue. The game thread enqueues, the worker
// thread dequeues, executes and rate-limits. Never blocks the game thread
// (enqueue is a fast locked push).
class DiscordQueue
{
public:
    void Push(DiscordOutMessage msg);
    bool Pop(DiscordOutMessage& out, uint32 timeoutMs);

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<DiscordOutMessage> messages_;
};

#endif // DISCORD_QUEUE_H_