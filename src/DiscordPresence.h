#ifndef DISCORD_PRESENCE_H_
#define DISCORD_PRESENCE_H_

#include "Define.h"
#include <mutex>
#include <string>

// Builds and refreshes the bot's Discord presence/activity text. The gateway
// reads GetStatusText() on its own cadence; Update() re-renders the text when
// the configured interval elapses so population numbers stay fresh.
// Game-thread owned; GetStatusText() is safe to call from the gateway thread.
class DiscordPresence
{
public:
    void Initialize();
    void Update(uint32 diff);

    std::string GetStatusText() const;
    std::string GetActivityType() const;

    bool Enabled() const { return enabled_; }

private:
    std::string RenderText() const;

    bool enabled_ = true;
    std::string activityType_ = "Playing";
    std::string template_;
    mutable std::mutex mutex_;
    std::string cached_;
    uint32 interval_ = 60;
    uint32 timer_ = 0;
};

#endif // DISCORD_PRESENCE_H_