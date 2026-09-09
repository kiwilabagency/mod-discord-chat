#ifndef DISCORD_SERVER_STATUS_H_
#define DISCORD_SERVER_STATUS_H_

#include "Define.h"
#include <cstdint>
#include <string>

// Maintains one persistent Discord server-status message in a channel. Rather
// than posting a new message each interval, the message is edited in place and
// its ID is persisted in the characters DB so it survives restarts.
// Game-thread owned; the Discord REST calls are enqueued to the worker thread.
class DiscordServerStatus
{
public:
    void Initialize();
    void Update(uint32 diff);

    bool Enabled() const { return enabled_; }

private:
    void SendOrUpdate();

    bool enabled_ = false;
    uint64_t channelId_ = 0;
    uint64_t messageId_ = 0;
    uint32 interval_ = 60;
    uint32 timer_ = 0;
};

#endif // DISCORD_SERVER_STATUS_H_