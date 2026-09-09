#include "DiscordMgr.h"
#include "DiscordClient.h"
#include "DiscordEmojiManager.h"
#include "DiscordPlayerProvider.h"
#include "DiscordChatBridge.h"
#include "DiscordAdmin.h"
#include "DiscordEvents.h"
#include "DiscordFilter.h"
#include "DiscordPresence.h"
#include "DiscordServerStatus.h"
#include "DiscordJson.h"
#include "GlobalChatProvider.h"
#include "GameTime.h"
#include "Log.h"
#include "Timer.h"
#include "World.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <sstream>

DiscordMgr::~DiscordMgr()
{
    // All subsystem unique_ptrs are destroyed here (types are complete in this TU).
}

DiscordMgr* DiscordMgr::instance()
{
    static DiscordMgr mgr;
    return &mgr;
}

void DiscordMgr::Initialize()
{
    if (initialized_)
        return;

    config_.Load();

    if (!config_.Enable)
    {
        LOG_INFO("modules.discord", "Discord Integration: Disabled (Discord.Enable = 0)");
        return;
    }

    if (config_.Token.empty())
    {
        LOG_ERROR("modules.discord", "Discord Integration: Discord.Bot.Token is not set. "
                  "The module will stay dormant. Set a valid bot token in discord-integration.conf.");
        return;
    }
    if (config_.GuildId == 0)
    {
        LOG_WARN("modules.discord", "Discord Integration: Discord.Bot.GuildId is not set. "
                 "Slash commands and emoji resolution will not work.");
    }

    LOG_INFO("modules.discord", "Discord Integration: initializing...");

    emojis_ = std::make_unique<DiscordEmojiManager>();
    playerProvider_ = std::make_unique<DiscordPlayerProvider>();
    bridge_ = std::make_unique<DiscordChatBridge>();
    admin_ = std::make_unique<DiscordAdmin>();
    events_ = std::make_unique<DiscordEvents>();
    presence_ = std::make_unique<DiscordPresence>();
    serverStatus_ = std::make_unique<DiscordServerStatus>();
    filter_ = std::make_unique<DiscordFilter>();

    emojis_->Initialize();
    playerProvider_->Initialize();
    bridge_->Initialize();
    admin_->Initialize();
    events_->Initialize();
    presence_->Initialize();
    serverStatus_->Initialize();
    filter_->Load();
    sGlobalChatProviderMgr->Initialize();

    // Build the networking client on the worker thread.
    client_ = std::make_shared<DiscordClient>(&config_);
    client_->SetGatewayEventHandler([this](DiscordGatewayEvent const& ev) {
        // Runs on the worker thread: just marshal onto the game thread.
        PostToGameThread([this, ev]() { HandleGatewayEvent(ev); });
    });
    client_->Worker().SetPresenceProviders(
        [this]() { return presence_->GetStatusText(); },
        [this]() { return presence_->GetActivityType(); });
    client_->Start();

    // Slash command registration + guild emoji fetch happen immediately.
    admin_->RegisterCommands();
    emojis_->Refresh();
    RefreshRoles();
    GetDiscordQuickSender().Start();

    enabled_ = true;
    initialized_ = true;

    PrintStartupDiagnostics();
}

void DiscordMgr::ReloadConfig()
{
    if (!initialized_)
        return;

    config_.Load();
    LOG_INFO("modules.discord", "Discord Integration: configuration reloaded.");

    // Refresh subsystems that depend on config.
    emojis_->Refresh();
    presence_->Initialize();
    serverStatus_->Initialize();
    filter_->Load();
    sGlobalChatProviderMgr->Initialize();
    admin_->RegisterCommands();
}

void DiscordMgr::Shutdown()
{
    if (!initialized_)
        return;

    // The shutdown/restart event itself is posted from OnShutdownInitiate (the
    // client is still running at that point). Here we just tear down cleanly.
    GetDiscordQuickSender().Stop();
    if (client_)
    {
        client_->Stop();
        client_.reset();
    }

    admin_->Shutdown();
    events_->Shutdown();
    bridge_->Shutdown();

    enabled_ = false;
    initialized_ = false;
}

void DiscordMgr::PostToGameThread(std::function<void()>&& fn)
{
    std::lock_guard<std::mutex> lock(queueMutex_);
    queue_.push_back(std::move(fn));
}

void DiscordMgr::PostToGameThreadDelayed(uint32 ms, std::function<void()> fn)
{
    std::lock_guard<std::mutex> lock(delayedMutex_);
    DelayedTask task;
    task.fireMs = getMSTime() + ms;
    task.fn = std::move(fn);
    delayedTasks_.push_back(std::move(task));
}

void DiscordMgr::ProcessDelayedTasks()
{
    std::vector<DelayedTask> due;
    {
        std::lock_guard<std::mutex> lock(delayedMutex_);
        uint32 now = getMSTime();
        for (auto it = delayedTasks_.begin(); it != delayedTasks_.end();)
        {
            if (it->fireMs <= now)
            {
                due.push_back(std::move(*it));
                it = delayedTasks_.erase(it);
            }
            else
                ++it;
        }
    }
    for (auto& task : due)
    {
        try
        {
            if (task.fn)
                task.fn();
        }
        catch (std::exception const& e)
        {
            LOG_WARN("modules.discord", "Discord delayed task error: {}", e.what());
        }
    }
}

void DiscordMgr::ProcessGameQueue()
{
    std::deque<std::function<void()>> local;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        local.swap(queue_);
    }
    for (auto& fn : local)
    {
        try
        {
            fn();
        }
        catch (std::exception const& e)
        {
            LOG_WARN("modules.discord", "Discord game-thread callback error: {}", e.what());
        }
    }
}

void DiscordMgr::Update(uint32 diff)
{
    if (!initialized_ || !enabled_)
        return;

    ProcessGameQueue();
    ProcessDelayedTasks();
    presence_->Update(diff);
    serverStatus_->Update(diff);
}

// ---------------------------------------------------------------------------
// Gateway event handling
// ---------------------------------------------------------------------------

void DiscordMgr::HandleGatewayEvent(DiscordGatewayEvent const& ev)
{
    DiscordJson::Value envelope;
    if (!DiscordJson::Parse(ev.data, envelope))
        return;
    if (DiscordJson::GetInt(envelope, "op") != 0)
        return;

    std::string eventName = DiscordJson::GetString(envelope, "t");
    DiscordJson::Value const* d = envelope.get("d");
    if (!d)
        return;

    if (eventName == "READY")
        HandleReady(*d);
    else if (eventName == "MESSAGE_CREATE")
        HandleMessageCreate(*d);
    else if (eventName == "INTERACTION_CREATE")
        HandleInteractionCreate(*d);
    else if (eventName == "GUILD_MEMBER_ADD" || eventName == "GUILD_MEMBER_REMOVE")
        HandleMemberEvent(eventName, *d);
    else if (eventName == "GUILD_EMOJIS_UPDATE")
        emojis_->ProcessEmojiUpdate(ev.data);
    else if (eventName == "GUILD_ROLES_UPDATE")
        RefreshRoles();
    else if (eventName == "GUILD_MEMBER_UPDATE")
    {
        // Role/color changes on an existing member: nothing cached to refresh here.
    }
}

void DiscordMgr::HandleReady(DiscordJson::Value const& d)
{
    if (client_)
        client_->SetConnected(true);
    LOG_INFO("modules.discord", "Bot: Connected");

    // Resolve the guild display name from the READY payload.
    std::string botUsername;
    if (DiscordJson::Value const* user = d.get("user"))
        botUsername = DiscordJson::GetString(*user, "username");
    if (DiscordJson::Value const* guilds = d.get("guilds"))
    {
        for (auto const& g : guilds->asArray())
        {
            uint64_t id = uint64_t(DiscordJson::GetInt(g, "id"));
            if (id == config_.GuildId)
            {
                guildName_ = DiscordJson::GetString(g, "name");
                break;
            }
        }
    }
    if (guildName_.empty())
        guildName_ = config_.ServerName;

    LOG_INFO("modules.discord", "Guild: {}", guildName_);
    LOG_INFO("modules.discord", "Bot User: {}", botUsername);

    // Emoji resolution is most reliable after the connection is established.
    emojis_->Refresh();
    RefreshRoles();

    // Announce server startup on the events channel.
    events_->OnServerStartup();
    PrintStartupDiagnostics();
}

void DiscordMgr::HandleMessageCreate(DiscordJson::Value const& d)
{
    DiscordChannelMessage msg;
    ParseChannelMessage(d, msg);
    if (!msg.channelId)
        return;
    bridge_->HandleMessageCreate(msg);
}

void DiscordMgr::HandleInteractionCreate(DiscordJson::Value const& d)
{
    DiscordInteraction ix;
    ParseInteraction(d, ix);
    if (!ix.id)
        return;
    admin_->HandleInteraction(ix);
}

void DiscordMgr::HandleMemberEvent(std::string const& eventName, DiscordJson::Value const& d)
{
    DiscordJson::Value const* user = d.get("user");
    if (!user)
        return;
    std::string name = DiscordJson::GetString(*user, "username");
    uint64_t userId = uint64_t(DiscordJson::GetInt(*user, "id"));
    if (eventName == "GUILD_MEMBER_ADD")
        events_->OnMemberJoin(name, userId);
    else
        events_->OnMemberLeave(name, userId);
}

// ---------------------------------------------------------------------------
// Parsing helpers
// ---------------------------------------------------------------------------

void DiscordMgr::ParseOptions(DiscordJson::Value const& opts, std::vector<DiscordCommandOption>& out)
{
    for (auto const& opt : opts.asArray())
    {
        DiscordCommandOption option;
        option.name = DiscordJson::GetString(opt, "name");
        long long type = DiscordJson::GetInt(opt, "type");
        if (type == 1) // SUB_COMMAND
        {
            option.isSubcommand = true;
            if (DiscordJson::Value const* nested = opt.get("options"))
                ParseOptions(*nested, option.options);
        }
        else if (type == 2) // SUB_COMMAND_GROUP
        {
            option.isSubcommandGroup = true;
            if (DiscordJson::Value const* nested = opt.get("options"))
                ParseOptions(*nested, option.options);
        }
        else if (DiscordJson::Value const* value = opt.get("value"))
        {
            if (value->kind == DiscordJson::Value::String)
                option.value = value->s;
            else if (value->kind == DiscordJson::Value::Int)
                option.value = std::to_string(value->i);
            else if (value->kind == DiscordJson::Value::Bool)
                option.value = value->b ? "1" : "0";
            else if (value->kind == DiscordJson::Value::Double)
                option.value = std::to_string(value->d);
        }
        out.push_back(std::move(option));
    }
}

void DiscordMgr::ParseInteraction(DiscordJson::Value const& d, DiscordInteraction& out)
{
    out.id = uint64_t(DiscordJson::GetInt(d, "id"));
    out.applicationId = uint64_t(DiscordJson::GetInt(d, "application_id"));
    out.channelId = uint64_t(DiscordJson::GetInt(d, "channel_id"));
    out.guildId = uint64_t(DiscordJson::GetInt(d, "guild_id"));
    out.token = DiscordJson::GetString(d, "token");

    long long type = DiscordJson::GetInt(d, "type");

    // Resolve user + roles (member object preferred).
    DiscordJson::Value const* user = d.get("user");
    if (DiscordJson::Value const* member = d.get("member"))
    {
        out.isMember = true;
        if (DiscordJson::Value const* mUser = member->get("user"))
            user = mUser;
        if (DiscordJson::Value const* roles = member->get("roles"))
            for (auto const& r : roles->asArray())
                if (r.kind == DiscordJson::Value::String)
                    out.roles.push_back(std::strtoull(r.s.c_str(), nullptr, 10));
    }
    if (user)
    {
        out.userId = uint64_t(DiscordJson::GetInt(*user, "id"));
        std::string username = DiscordJson::GetString(*user, "username");
        std::string discriminator = DiscordJson::GetString(*user, "discriminator");
        out.userTag = discriminator.empty() || discriminator == "0"
            ? username
            : username + "#" + discriminator;
    }

    if (type == 2) // slash command
    {
        if (DiscordJson::Value const* data = d.get("data"))
        {
            out.name = DiscordJson::GetString(*data, "name");
            if (DiscordJson::Value const* options = data->get("options"))
                ParseOptions(*options, out.options);
        }
    }
    else if (type == 3) // message component (button)
    {
        DiscordCommandOption comp;
        comp.name = "__component__";
        comp.value = DiscordJson::GetString(d, "custom_id");
        out.options.push_back(std::move(comp));
    }
}

void DiscordMgr::ParseChannelMessage(DiscordJson::Value const& d, DiscordChannelMessage& out)
{
    out.channelId = uint64_t(DiscordJson::GetInt(d, "channel_id"));
    out.content = DiscordJson::GetString(d, "content");

    // Slash-command responses (from the bot) are not chat; never relay them.
    if (d.get("interaction"))
    {
        out.isInteraction = true;
        return;
    }

    // Webhook messages carry a webhook_id and no author.
    if (DiscordJson::Value const* webhookId = d.get("webhook_id"))
    {
        out.fromWebhook = true;
        out.authorId = 0;
        return;
    }

    if (DiscordJson::Value const* author = d.get("author"))
    {
        out.authorId = uint64_t(DiscordJson::GetInt(*author, "id"));
        out.authorName = DiscordJson::GetString(*author, "global_name");
        if (out.authorName.empty())
            out.authorName = DiscordJson::GetString(*author, "username");
        out.authorIsBot = DiscordJson::GetBool(*author, "bot");
    }

    // Member context: guild nickname + role ids.
    if (DiscordJson::Value const* member = d.get("member"))
    {
        out.nickname = DiscordJson::GetString(*member, "nick");
        if (DiscordJson::Value const* roles = member->get("roles"))
            for (auto const& r : roles->asArray())
                if (r.kind == DiscordJson::Value::String)
                    out.roles.push_back(std::strtoull(r.s.c_str(), nullptr, 10));
    }

    if (DiscordJson::Value const* attachments = d.get("attachments"))
        out.hasAttachments = !attachments->asArray().empty();

    // Flatten replies into readable game text.
    if (DiscordJson::Value const* ref = d.get("message_reference"))
    {
        if (DiscordJson::Value const* referenced = d.get("referenced_message"))
        {
            if (DiscordJson::Value const* refAuthor = referenced->get("author"))
                out.referencedAuthor = DiscordJson::GetString(*refAuthor, "username");
        }
    }
}

// ---------------------------------------------------------------------------
// Diagnostics / text helpers
// ---------------------------------------------------------------------------

std::string DiscordMgr::BuildStatusText(bool includeLastUpdate) const
{
    DiscordPopulation pop = playerProvider_->GetPopulation();
    std::string realm = config_.ServerName.empty() ? "AzerothCore" : config_.ServerName;

    std::string text = "**Status:** " + std::string(client_ && client_->IsConnected() ? "Online" : "Offline") + "\n" +
                       "**Realm:** " + realm + "\n" +
                       "**Players:** " + std::to_string(pop.real);
    if (config_.ShowPopulation && pop.bots > 0)
    {
        text += "\n**Playerbots:** " + std::to_string(pop.bots) + "\n" +
                "**Total:** " + std::to_string(pop.total);
    }
    text += "\n**Uptime:** " + FormatUptime(uint32(GameTime::GetUptime().count())) + "\n" +
            "**Expansion:** WotLK 3.3.5a";
    if (includeLastUpdate)
    {
        time_t now = time(nullptr);
        tm tmv;
        localtime_s(&tmv, &now);
        char buf[32];
        strftime(buf, sizeof(buf), "%H:%M:%S", &tmv);
        text += "\n**Last update:** " + std::string(buf);
    }
    return text;
}

std::string DiscordMgr::FormatUptime(uint32 seconds)
{
    uint32 days = seconds / 86400;
    uint32 hours = (seconds % 86400) / 3600;
    uint32 minutes = (seconds % 3600) / 60;
    std::ostringstream ss;
    if (days) ss << days << "d ";
    if (hours || days) ss << hours << "h ";
    ss << minutes << "m";
    return ss.str();
}

void DiscordMgr::PrintStartupDiagnostics()
{
    LOG_INFO("modules.discord", "Discord Integration");
    LOG_INFO("modules.discord", "Bot: {}", client_ && client_->IsConnected() ? "Connected" : "Starting");
    LOG_INFO("modules.discord", "Guild: {}", guildName_.empty() ? std::to_string(config_.GuildId) : guildName_);
    LOG_INFO("modules.discord", "Global Chat: {}", config_.GlobalChatEnable ? "Enabled" : "Disabled");
    LOG_INFO("modules.discord", "Provider: {}", sGlobalChatProviderMgr->GetSelectedName());
    LOG_INFO("modules.discord", "Global Channel: {}", config_.GlobalChatEnable ? "Found" : "Not configured");
    LOG_INFO("modules.discord", "Admin Channel: {}", config_.AdminChannelId ? "Found" : "Not configured");
    LOG_INFO("modules.discord", "Events Channel: {}",
             (config_.EventsChannelId || !config_.WebhookEvents.empty()) ? "Found" : "Not configured");
    LOG_INFO("modules.discord", "Status Channel: {}", config_.StatusChannelId ? "Found" : "Not configured");
    LOG_INFO("modules.discord", "Webhook: {}", config_.UseWebhooks ? "Ready" : "Disabled");
    LOG_INFO("modules.discord", "Slash Commands: Registered");
    LOG_INFO("modules.discord", "Presence: {}", presence_->Enabled() ? "Enabled" : "Disabled");
    LOG_INFO("modules.discord", "Playerbots: {}", playerProvider_->PlayerbotsAvailable() ? "Detected" : "Not detected");

    if (config_.SplitFactions)
    {
        if (!config_.AllianceChannelId || !config_.HordeChannelId)
            LOG_WARN("modules.discord", "SplitFactions is enabled but Alliance/Horde channel IDs are incomplete.");
    }
    else if (!config_.GlobalChannelId)
    {
        LOG_WARN("modules.discord", "Discord.GlobalChat.ChannelId is not set; "
                 "Discord chat will not bridge until a channel is configured.");
    }
    if (config_.GlobalChatEnable && !sGlobalChatProviderMgr->GetProvider())
    {
        LOG_WARN("modules.discord", "No Global Chat provider detected; "
                 "Discord -> game messages will not be delivered.");
    }
}

// ---------------------------------------------------------------------------
// Discord role handling
// ---------------------------------------------------------------------------

void DiscordMgr::RefreshRoles()
{
    if (!config_.GuildId)
        return;
    auto client = client_.get();
    if (!client)
        return;
    client->GetGuildRoles([this](std::string const& json) {
        auto* mgr = this;
        PostToGameThread([mgr, json]() { mgr->ProcessGuildRoles(json); });
    });
}

void DiscordMgr::ProcessGuildRoles(std::string const& json)
{
    roleCache_.clear();

    DiscordJson::Value root;
    if (!DiscordJson::Parse(json, root) || root.kind != DiscordJson::Value::Array)
        return;

    for (auto const& r : root.asArray())
    {
        DiscordRoleInfo info;
        info.id = uint64_t(DiscordJson::GetInt(r, "id"));
        info.name = DiscordJson::GetString(r, "name");
        info.color = uint32_t(DiscordJson::GetInt(r, "color"));
        info.position = int32_t(DiscordJson::GetInt(r, "position"));
        info.valid = info.id != 0;
        if (info.valid)
            roleCache_[info.id] = info;
    }
    LOG_INFO("modules.discord.roles", "Discord roles loaded: {}", roleCache_.size());
}

DiscordRoleInfo const* DiscordMgr::GetRoleInfo(uint64_t roleId) const
{
    auto it = roleCache_.find(roleId);
    return it == roleCache_.end() ? nullptr : &it->second;
}

std::vector<std::string> DiscordMgr::ResolveMemberTags(std::vector<uint64_t> const& roles) const
{
    std::vector<std::string> tags;
    if (!config_.RolesEnabled)
        return tags;

    for (auto const& [roleId, label] : config_.RoleTags)
    {
        for (uint64_t memberRole : roles)
        {
            if (memberRole != roleId)
                continue;
            std::string display = label;
            if (display.empty())
            {
                if (DiscordRoleInfo const* info = GetRoleInfo(roleId))
                    display = info->name;
            }
            if (!display.empty())
                tags.push_back(display);
            break;
        }
    }
    return tags;
}

void DiscordMgr::ResolveMemberIcons(std::vector<uint64_t> const& roles, std::string& factionIcon,
                                    std::string& classIcon, uint8& classId) const
{
    factionIcon.clear();
    classIcon.clear();
    classId = DiscordChat::INVALID_CLASS;

    if (!config_.RolesEnabled)
        return;

    auto classIconTexture = [](uint8 cls) -> char const* {
        switch (cls)
        {
            case CLASS_WARRIOR: return "INV_Sword_27";
            case CLASS_PALADIN: return "INV_Hammer_01";
            case CLASS_HUNTER: return "INV_Weapon_Bow_07";
            case CLASS_ROGUE: return "INV_ThrowingKnife_04";
            case CLASS_PRIEST: return "INV_Staff_30";
            case CLASS_DEATH_KNIGHT: return "Spell_Deathknight_ClassIcon";
            case CLASS_SHAMAN: return "Spell_Nature_BloodLust";
            case CLASS_MAGE: return "INV_Staff_13";
            case CLASS_WARLOCK: return "Spell_Nature_FaerieFire";
            case CLASS_DRUID: return "Ability_Druid_Maul";
            default: return nullptr;
        }
    };

    for (auto const& [roleId, label] : config_.RoleTags)
    {
        if (std::find(roles.begin(), roles.end(), roleId) == roles.end())
            continue;

        std::string tag = label;
        if (tag.empty())
        {
            if (DiscordRoleInfo const* info = GetRoleInfo(roleId))
                tag = info->name;
        }
        std::string key;
        for (char c : tag)
        {
            if (c == ' ' || c == '\t')
                continue;
            key += char(std::tolower(static_cast<unsigned char>(c)));
        }

        if (key == "alliance" && factionIcon.empty())
            factionIcon = "|TInterface\\pvpframe\\pvp-currency-alliance:17|t|r";
        else if (key == "horde" && factionIcon.empty())
            factionIcon = "|TInterface\\pvpframe\\pvp-currency-horde:17|t|r";
        else if (classId == DiscordChat::INVALID_CLASS)
        {
            uint8 cls = DiscordChat::INVALID_CLASS;
            if (key == "warrior") cls = CLASS_WARRIOR;
            else if (key == "paladin") cls = CLASS_PALADIN;
            else if (key == "hunter") cls = CLASS_HUNTER;
            else if (key == "rogue") cls = CLASS_ROGUE;
            else if (key == "priest") cls = CLASS_PRIEST;
            else if (key == "deathknight") cls = CLASS_DEATH_KNIGHT;
            else if (key == "shaman") cls = CLASS_SHAMAN;
            else if (key == "mage") cls = CLASS_MAGE;
            else if (key == "warlock") cls = CLASS_WARLOCK;
            else if (key == "druid") cls = CLASS_DRUID;

            if (cls != DiscordChat::INVALID_CLASS)
            {
                classId = cls;
                if (char const* icon = classIconTexture(cls))
                    classIcon = std::string("|TInterface\\icons\\") + icon + ":15|t|r";
            }
        }
    }
}

DiscordMemberStyle DiscordMgr::ResolveMemberStyle(std::vector<uint64_t> const& roles) const
{
    DiscordMemberStyle style;
    if (!config_.RolesEnabled)
        return style;

    bool hasAdmin = false;
    std::vector<DiscordRoleInfo const*> candidates;
    for (uint64_t roleId : roles)
    {
        if (config_.DisplayAdminRoleIds.count(roleId))
            hasAdmin = true;
        if (DiscordRoleInfo const* info = GetRoleInfo(roleId))
            candidates.push_back(info);
    }

    // Admin roles: forced red + Blizz icon.
    if (hasAdmin)
    {
        style.isAdmin = true;
        style.color = config_.AdminColor;
        style.hasColoredRole = true;
        return style;
    }

    // Otherwise use the member's highest-position role with a color
    // (config override first, then the live Discord role color).
    std::sort(candidates.begin(), candidates.end(),
              [](DiscordRoleInfo const* a, DiscordRoleInfo const* b) { return a->position > b->position; });
    for (DiscordRoleInfo const* role : candidates)
    {
        auto overrideIt = config_.RoleColorOverrides.find(role->id);
        if (overrideIt != config_.RoleColorOverrides.end())
        {
            style.color = overrideIt->second;
            style.hasColoredRole = true;
            break;
        }
        if (config_.UseLiveRoleColors && role->color != 0)
        {
            style.color = role->color;
            style.hasColoredRole = true;
            break;
        }
    }
    return style;
}