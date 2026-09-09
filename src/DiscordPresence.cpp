#include "DiscordPresence.h"
#include "DiscordConfig.h"
#include "DiscordMgr.h"
#include "DiscordPlayerProvider.h"
#include "GameTime.h"
#include "World.h"

void DiscordPresence::Initialize()
{
    DiscordConfig const* cfg = sDiscordMgr->Config();
    enabled_ = cfg && cfg->PresenceEnable;
    if (!cfg)
        return;
    activityType_ = cfg->PresenceActivityType;
    template_ = cfg->PresenceText;
    interval_ = cfg->PresenceUpdateInterval;
    timer_ = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    cached_ = RenderText();
}

void DiscordPresence::Update(uint32 diff)
{
    if (!enabled_)
        return;
    timer_ += diff;
    if (timer_ >= interval_ * 1000)
    {
        timer_ = 0;
        std::string text;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            text = RenderText();
            cached_ = text;
        }
    }
}

std::string DiscordPresence::RenderText() const
{
    DiscordConfig const* cfg = sDiscordMgr->Config();
    DiscordPopulation pop = sDiscordMgr->PlayerProvider()->GetPopulation();

    std::string realm = cfg ? cfg->ServerName : "AzerothCore";
    std::string players = std::to_string(pop.real);
    std::string playerbots = std::to_string(pop.bots);
    std::string total = std::to_string(pop.total);
    std::string uptime = sDiscordMgr->FormatUptime(uint32(GameTime::GetUptime().count()));

    std::string out = template_;
    auto replace = [&out](std::string const& var, std::string const& value) {
        size_t pos;
        while ((pos = out.find(var)) != std::string::npos)
            out.replace(pos, var.size(), value);
    };
    replace("{realm}", realm);
    replace("{players}", players);
    replace("{playerbots}", playerbots);
    replace("{total}", total);
    replace("{uptime}", uptime);
    replace("{expansion}", "WotLK 3.3.5a");
    return out;
}

std::string DiscordPresence::GetStatusText() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return cached_.empty() ? (enabled_ ? RenderText() : std::string()) : cached_;
}

std::string DiscordPresence::GetActivityType() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return activityType_;
}