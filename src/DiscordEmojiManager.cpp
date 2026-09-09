#include "DiscordEmojiManager.h"
#include "DiscordClient.h"
#include "DiscordJson.h"
#include "DiscordMgr.h"
#include "Log.h"
#include "Player.h"

#include <cctype>
#include <cstdlib>

namespace
{
    std::string StripColons(std::string name)
    {
        if (name.size() >= 2 && name.front() == ':' && name.back() == ':')
            name = name.substr(1, name.size() - 2);
        return name;
    }

    uint64 ExtractEmojiId(std::string const& emoji)
    {
        size_t lastColon = emoji.rfind(':');
        if (lastColon == std::string::npos)
            return 0;

        std::string id = emoji.substr(lastColon + 1);
        if (!id.empty() && id.back() == '>')
            id.pop_back();

        return std::strtoull(id.c_str(), nullptr, 10);
    }
}

std::string DiscordEmojiManager::NormalizeName(std::string name)
{
    name = StripColons(name);
    for (char& c : name)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return name;
}

void DiscordEmojiManager::Initialize()
{
    config_ = sDiscordMgr->Config();
    if (!config_ || !config_->EmojisEnable)
    {
        LOG_INFO("modules.discord.emoji", "Discord Emojis: Disabled");
        return;
    }
    Refresh();
}

void DiscordEmojiManager::Refresh()
{
    if (!config_ || !config_->EmojisEnable)
        return;
    auto client = sDiscordMgr->Client();
    if (!client)
        return; // client not ready yet; Refresh is re-invoked after connect
    if (!config_->GuildId && !config_->ApplicationId)
    {
        LOG_WARN("modules.discord.emoji", "Discord emojis require Discord.Bot.GuildId or "
                 "Discord.Bot.ApplicationId to be set.");
        return;
    }

    nameToEmoji_.clear();
    unresolved_.clear();
    appEmojiIds_.clear();
    guildEmojiIds_.clear();
    appEmojiCount_ = 0;

    // Fetch the bot application's own emojis first (these work in any guild the
    // bot joins), then the guild's emojis (guild emojis override same-named
    // application emojis so each server can keep its own).
    client->GetApplicationEmojis([this](std::string const& json) {
        auto* mgr = this;
        sDiscordMgr->PostToGameThread([mgr, json]() { mgr->ProcessApplicationEmojis(json); });
    });
    client->GetGuildEmojis([this](std::string const& json) {
        auto* mgr = this;
        sDiscordMgr->PostToGameThread([mgr, json]() { mgr->ProcessGuildEmojis(json); });
    });
}

void DiscordEmojiManager::ProcessApplicationEmojis(std::string const& json)
{
    // Merge application emojis; guild emojis are merged on top later.
    std::unordered_map<std::string, std::string> app;
    DiscordJson::Value root;
    if (DiscordJson::Parse(json, root) && root.kind == DiscordJson::Value::Array)
    {
        for (auto const& emoji : root.asArray())
        {
            std::string id = DiscordJson::GetString(emoji, "id");
            std::string name = DiscordJson::GetString(emoji, "name");
            if (id.empty() || name.empty())
                continue;
            bool animated = DiscordJson::GetBool(emoji, "animated");
            app[NormalizeName(name)] =
                animated ? "<a:" + name + ":" + id + ">" : "<:" + name + ":" + id + ">";
            appEmojiIds_.insert(std::strtoull(id.c_str(), nullptr, 10));
        }
    }
    for (auto const& [name, emoji] : app)
        nameToEmoji_[name] = emoji;
    appEmojiCount_ = uint32(appEmojiIds_.size());
    LOG_INFO("modules.discord.emoji", "Application Emojis: {} loaded", appEmojiCount_);
    ResolveAll();
}

void DiscordEmojiManager::ProcessGuildEmojis(std::string const& json)
{
    // Guild emojis override application emojis with the same name.
    DiscordJson::Value root;
    if (DiscordJson::Parse(json, root) && root.kind == DiscordJson::Value::Array)
    {
        for (auto const& emoji : root.asArray())
        {
            std::string id = DiscordJson::GetString(emoji, "id");
            std::string name = DiscordJson::GetString(emoji, "name");
            if (id.empty() || name.empty())
                continue;
            bool animated = DiscordJson::GetBool(emoji, "animated");
            nameToEmoji_[NormalizeName(name)] =
                animated ? "<a:" + name + ":" + id + ">" : "<:" + name + ":" + id + ">";
            guildEmojiIds_.insert(std::strtoull(id.c_str(), nullptr, 10));
        }
    }
    fetched_ = true;
    ResolveAll();
    PrintStartupDiagnostics();
}

bool DiscordEmojiManager::ContainsAppEmoji(std::string const& text) const
{
    for (size_t i = 0; i + 1 < text.size(); ++i)
    {
        if (text[i] != '<')
            continue;
        size_t close = text.find('>', i);
        if (close == std::string::npos)
            break;
        std::string mention = text.substr(i + 1, close - i - 1); // e.g. "a:name:id" or ":name:id"
        size_t lastColon = mention.rfind(':');
        if (lastColon != std::string::npos)
        {
            try
            {
                uint64_t id = std::strtoull(mention.substr(lastColon + 1).c_str(), nullptr, 10);
                if (IsAppOnlyEmojiId(id))
                    return true;
            }
            catch (...) {}
        }
        i = close;
    }
    return false;
}

std::string DiscordEmojiManager::RewriteAppEmojisToGuild(std::string const& text) const
{
    std::string out;
    out.reserve(text.size());

    for (size_t i = 0; i < text.size();)
    {
        if (text[i] != '<')
        {
            out += text[i++];
            continue;
        }

        size_t close = text.find('>', i);
        if (close == std::string::npos)
        {
            out.append(text, i, std::string::npos);
            break;
        }

        std::string full = text.substr(i, close - i + 1);
        std::string mention = full.substr(1, full.size() - 2);
        size_t firstColon = mention.find(':');
        size_t lastColon = mention.rfind(':');
        if (firstColon != std::string::npos && lastColon != std::string::npos && firstColon < lastColon)
        {
            uint64 emojiId = ExtractEmojiId(mention);
            if (IsAppOnlyEmojiId(emojiId))
            {
                std::string name = mention.substr(firstColon + 1, lastColon - firstColon - 1);
                auto it = nameToEmoji_.find(NormalizeName(name));
                if (it != nameToEmoji_.end() && guildEmojiIds_.count(ExtractEmojiId(it->second)))
                {
                    out += it->second;
                    i = close + 1;
                    continue;
                }
            }
        }

        out += full;
        i = close + 1;
    }

    return out;
}

void DiscordEmojiManager::ProcessEmojiUpdate(std::string const& /*json*/)
{
    // GUILD_EMOJIS_UPDATE fired: the emoji list changed. Simply re-fetch it.
    Refresh();
}

std::string DiscordEmojiManager::ResolveConfigured(std::string const& configured) const
{
    // If the value is already a full custom emoji (<:name:id> or <a:name:id>) pass it through.
    std::string trimmed = configured;
    // trim whitespace
    size_t b = trimmed.find_first_not_of(" \t");
    if (b != std::string::npos)
        trimmed = trimmed.substr(b);
    if (trimmed.size() >= 3 && trimmed[0] == '<' && trimmed.back() == '>')
        return trimmed;

    std::string key = NormalizeName(trimmed);
    auto it = nameToEmoji_.find(key);
    if (it != nameToEmoji_.end())
        return it->second;
    return "";
}

bool DiscordEmojiManager::IsAppOnlyEmojiId(uint64_t id) const
{
    return id && appEmojiIds_.count(id) && !guildEmojiIds_.count(id);
}

void DiscordEmojiManager::ResolveAll()
{
    resolvedClassCount_ = 0;
    resolvedRaceCount_ = 0;
    resolvedFactionCount_ = 0;
    unresolved_.clear();

    auto resolve = [this](std::string const& configured) {
        std::string out = ResolveConfigured(configured);
        if (out.empty())
            unresolved_[NormalizeName(configured)] = "not found";
        return out;
    };

    resolvedFaction_[TEAM_ALLIANCE] = resolve(config_->EmojiFactionAlliance);
    resolvedFaction_[TEAM_HORDE] = resolve(config_->EmojiFactionHorde);
    resolvedFactionCount_ = uint32(!resolvedFaction_[TEAM_ALLIANCE].empty()) + uint32(!resolvedFaction_[TEAM_HORDE].empty());

    resolvedGm_ = resolve(config_->EmojiGm);

    resolvedClass_.clear();
    for (auto const& [classId, name] : config_->EmojiClass)
    {
        std::string out = resolve(name);
        if (!out.empty())
            ++resolvedClassCount_;
        resolvedClass_[classId] = out;
    }

    resolvedRace_.clear();
    for (auto const& [raceId, pair] : config_->EmojiRace)
    {
        std::string male = resolve(pair.first);
        std::string female = resolve(pair.second);
        if (!male.empty() && !female.empty())
            ++resolvedRaceCount_;
        resolvedRace_[raceId] = {male, female};
    }

    for (auto const& [name, reason] : unresolved_)
    {
        LOG_WARN("modules.discord.emoji", "Discord emoji '{}' was not found in guild {} ({}). The emoji will be skipped.",
                 name, config_->GuildId, reason);
    }
}

void DiscordEmojiManager::PrintStartupDiagnostics() const
{
    LOG_INFO("modules.discord.emoji", "Discord Emojis: Enabled");
    if (config_->EmojisFactionEnable)
        LOG_INFO("modules.discord.emoji", "Faction Emojis: Enabled - {}/2 resolved", resolvedFactionCount_);
    else
        LOG_INFO("modules.discord.emoji", "Faction Emojis: Disabled");

    if (config_->EmojisClassEnable)
        LOG_INFO("modules.discord.emoji", "Class Emojis: Enabled - {}/{} resolved", resolvedClassCount_, config_->EmojiClass.size());
    else
        LOG_INFO("modules.discord.emoji", "Class Emojis: Disabled");

    if (config_->EmojisRaceEnable)
        LOG_INFO("modules.discord.emoji", "Race Emojis: Enabled - {}/{} resolved", resolvedRaceCount_, config_->EmojiRace.size());
    else
        LOG_INFO("modules.discord.emoji", "Race Emojis: Disabled");
}

std::string DiscordEmojiManager::RenderFaction(uint32 team) const
{
    if (!config_ || !config_->EmojisEnable || !config_->EmojisFactionEnable || !config_->EmojisShowFaction)
        return "";
    if (team > 1)
        return "";
    return resolvedFaction_[team];
}

std::string DiscordEmojiManager::RenderClass(uint8 classId) const
{
    if (!config_ || !config_->EmojisEnable || !config_->EmojisClassEnable || !config_->EmojisShowClass)
        return "";
    auto it = resolvedClass_.find(classId);
    if (it == resolvedClass_.end())
        return "";
    return it->second;
}

std::string DiscordEmojiManager::RenderRace(uint8 raceId, uint8 gender) const
{
    if (!config_ || !config_->EmojisEnable || !config_->EmojisRaceEnable || !config_->EmojisShowRace)
        return "";
    auto it = resolvedRace_.find(raceId);
    if (it == resolvedRace_.end())
        return "";
    return gender == GENDER_FEMALE ? it->second.second : it->second.first;
}

std::string DiscordEmojiManager::RenderGm() const
{
    if (!config_ || !config_->EmojisEnable)
        return "";
    return resolvedGm_;
}

std::string DiscordEmojiManager::RenderPrefix(DiscordPlayerIdentity const& identity) const
{
    if (!config_ || !config_->EmojisEnable || !identity.valid)
        return "";

    std::string prefix;
    auto append = [&prefix](std::string const& part) {
        if (!part.empty())
        {
            if (!prefix.empty())
                prefix += " ";
            prefix += part;
        }
    };

    append(RenderFaction(identity.team));
    append(RenderRace(identity.race, identity.gender));
    append(RenderClass(identity.cls));
    return prefix;
}