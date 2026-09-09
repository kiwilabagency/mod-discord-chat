#include "DiscordChatBridge.h"
#include "DiscordClient.h"
#include "DiscordConfig.h"
#include "DiscordEmojiManager.h"
#include "DiscordFilter.h"
#include "DiscordJson.h"
#include "DiscordMgr.h"
#include "DiscordPlayerProvider.h"
#include "GameTime.h"
#include "GlobalChatProvider.h"
#include "Log.h"
#include "Player.h"
#include "WorldSessionMgr.h"
#include "WorldSession.h"
#include "Channel.h"
#include "ChannelMgr.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <thread>

namespace
{
    // ---- Fast-path sender ------------------------------------------------

    std::string Hex6(uint32 value)
    {
        char buf[8];
        snprintf(buf, sizeof(buf), "%06X", value);
        return std::string(buf);
    }

    bool StartsWithCI(std::string const& s, std::string const& prefix)
    {
        if (s.size() < prefix.size())
            return false;
        for (size_t i = 0; i < prefix.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
                return false;
        return true;
    }

    // "<:name:id>" or "<a:name:id>" -> ":name:" ; also escape '|' so Discord
    // users cannot inject WoW color codes into the in-game chat.
    std::string ConvertCustomEmojisToNames(std::string const& text)
    {
        std::string out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size();)
        {
            if (text[i] == '|')
            {
                out += "||";
                ++i;
                continue;
            }
            if (text[i] == '<' && i + 2 < text.size() && (text[i + 1] == ':' || text[i + 1] == 'a'))
            {
                size_t j = text.find('>', i);
                if (j != std::string::npos)
                {
                    std::string token = text.substr(i + 1, j - i - 1);
                    size_t nameStart = (token[0] == 'a') ? 2 : 1;
                    size_t colon = token.find(':', nameStart);
                    if (colon != std::string::npos)
                    {
                        std::string name = token.substr(nameStart, colon - nameStart);
                        out += ":" + name + ":";
                        i = j + 1;
                        continue;
                    }
                }
            }
            out += text[i];
            ++i;
        }
        return out;
    }

    // Remove @everyone / @here / <@user> / <@&role> mentions before the text is
    // shown in-game (Discord mention syntax is meaningless in WoW chat).
    std::string StripDiscordMentions(std::string const& text)
    {
        std::string out = text;
        auto removeAll = [&out](std::string const& needle) {
            size_t pos;
            while ((pos = out.find(needle)) != std::string::npos)
                out.erase(pos, needle.size());
        };
        removeAll("@everyone");
        removeAll("@here");

        std::string filtered;
        filtered.reserve(out.size());
        for (size_t i = 0; i < out.size();)
        {
            if (out[i] == '<' && i + 2 < out.size() && (out[i + 1] == '@'))
            {
                size_t j = out.find('>', i);
                if (j != std::string::npos)
                {
                    i = j + 1; // drop <@...> and <@&...>
                    continue;
                }
            }
            filtered += out[i];
            ++i;
        }
        return filtered;
    }
}

DiscordQuickSender& GetDiscordQuickSender()
{
    static DiscordQuickSender sender;
    return sender;
}

void DiscordQuickSender::Start()
{
    if (running_.exchange(true))
        return;
    thread_ = std::thread(&DiscordQuickSender::Run, this);
}

void DiscordQuickSender::Stop()
{
    if (!running_.exchange(false))
        return;
    if (thread_.joinable())
        thread_.join();
}

void DiscordQuickSender::Send(DiscordOutMessage msg)
{
    queue_.Push(std::move(msg));
}

void DiscordQuickSender::Run()
{
    LOG_INFO("modules.discord.quick", "Discord quick-send worker thread started.");
    DiscordOutMessage msg;
    while (running_)
    {
        if (!queue_.Pop(msg, 500))
            continue;

        std::string url = msg.isWebhook
            ? msg.path
            : std::string("https://") + DiscordChat::API_HOST + DiscordChat::API_VERSION + msg.path;

        std::string token;
        if (auto client = sDiscordMgr->Client())
            token = client->Token();

        std::string responseBody;
        long long responseCode = 0;
        int status = http_.Request(url, msg.method, msg.payload, token, msg.needsAuth,
                                   msg.isWebhook, false, &responseBody, &responseCode);

        if (status == 429 && !msg.retried)
        {
            msg.retried = true;
            long long backoff = 250;
            DiscordJson::Value parsed;
            if (DiscordJson::Parse(responseBody, parsed))
            {
                long long retryAfter = DiscordJson::GetInt(parsed, "retry_after");
                if (retryAfter > 0)
                    backoff = std::min<long long>(retryAfter * 1000LL, 2000);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
            queue_.Push(std::move(msg));
            continue;
        }

        if (status < 200 || status >= 300)
        {
            std::string kind = msg.isWebhook ? "webhook" : "api";
            std::string body = responseBody.empty() ? "<empty>" : responseBody.substr(0, 300);
            LOG_WARN("modules.discord.quick", "Discord {} send failed ({} {} -> {}). Response: {}",
                     kind, msg.method, url, responseCode, body);
        }
    }
    LOG_INFO("modules.discord.quick", "Discord quick-send worker thread stopped.");
}

void DiscordChatBridge::Initialize()
{
    initialized_ = true;
}

void DiscordChatBridge::Shutdown()
{
    initialized_ = false;
}

uint64_t DiscordChatBridge::ResolveChatChannel(uint32 team) const
{
    DiscordConfig const* cfg = sDiscordMgr->Config();
    if (!cfg)
        return 0;
    if (cfg->SplitFactions)
        return team == TEAM_HORDE ? cfg->HordeChannelId : cfg->AllianceChannelId;
    return cfg->GlobalChannelId;
}

std::string DiscordChatBridge::ResolveWebhookUrl(uint32 team) const
{
    DiscordConfig const* cfg = sDiscordMgr->Config();
    if (!cfg)
        return "";
    uint64_t channelId = ResolveChatChannel(team);
    if (cfg->SplitFactions)
    {
        std::string configured = team == TEAM_HORDE ? cfg->WebhookHordeChat : cfg->WebhookAllianceChat;
        if (!configured.empty())
            return configured;
    }
    else if (!cfg->WebhookGlobalChat.empty())
        return cfg->WebhookGlobalChat;

    // No configured webhook: use the auto-created one (transparent avatar).
    auto it = autoWebhookUrls_.find(channelId);
    return it == autoWebhookUrls_.end() ? std::string() : it->second;
}

void DiscordChatBridge::EnsureWebhook(uint64_t channelId)
{
    if (!channelId)
        return;
    if (autoWebhookUrls_.count(channelId))
        return;

    uint32 now = uint32(GameTime::GetUptime().count());
    auto attempt = webhookAttemptTime_.find(channelId);
    if (attempt != webhookAttemptTime_.end() && now - attempt->second < 60)
        return; // retry at most once per minute
    webhookAttemptTime_[channelId] = now;

    auto client = sDiscordMgr->Client();
    if (!client)
        return;

    // Invisible username + transparent 1x1 avatar -> only the message content shows.
    static char const* kTransparentPng =
        "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAC0lEQVR42mNkYAAAAAYAAjCB0C8AAAAASUVORK5CYII=";

    client->CreateChannelWebhook(channelId, "\xE3\x85\xA4", kTransparentPng, [this, channelId](std::string const& json) {
        auto* self = this;
        sDiscordMgr->PostToGameThread([self, channelId, json]() {
            DiscordJson::Value parsed;
            if (DiscordJson::Parse(json, parsed))
            {
                std::string id = DiscordJson::GetString(parsed, "id");
                std::string token = DiscordJson::GetString(parsed, "token");
                if (!id.empty() && !token.empty())
                {
                    self->autoWebhookUrls_[channelId] =
                        "https://discord.com/api/webhooks/" + id + "/" + token;
                    LOG_INFO("modules.discord.bridge", "Auto webhook created for channel {} "
                             "(invisible name + transparent avatar).", channelId);
                    self->FlushPendingWebhookMessages(channelId, self->autoWebhookUrls_[channelId]);
                }
                else
                {
                    size_t dropped = 0;
                    auto pendingIt = self->pendingWebhookMessages_.find(channelId);
                    if (pendingIt != self->pendingWebhookMessages_.end())
                    {
                        dropped = pendingIt->second.size();
                        self->pendingWebhookMessages_.erase(pendingIt);
                    }
                    LOG_WARN("modules.discord.bridge", "Auto webhook creation failed for channel {} "
                             "(check the bot has the Manage Webhooks permission). Dropped {} queued "
                             "message(s) instead of falling back to bot posts.", channelId, dropped);
                }
            }
        });
    });
}

void DiscordChatBridge::QueuePendingWebhookMessage(uint64_t channelId, std::string const& username,
                                                   std::string const& prefix, std::string const& message,
                                                   uint32 color)
{
    auto& pending = pendingWebhookMessages_[channelId];
    if (pending.size() >= 20)
        pending.erase(pending.begin());

    pending.push_back({username, prefix, message, color});
}

void DiscordChatBridge::FlushPendingWebhookMessages(uint64_t channelId, std::string const& webhookUrl)
{
    auto it = pendingWebhookMessages_.find(channelId);
    if (it == pendingWebhookMessages_.end())
        return;

    auto pending = std::move(it->second);
    pendingWebhookMessages_.erase(it);

    for (PendingWebhookMessage const& msg : pending)
        SendToChannel(channelId, webhookUrl, msg.username, msg.prefix, msg.message, msg.color);
}

void DiscordChatBridge::SendToChannel(uint64_t channelId, std::string const& webhookUrl,
                                      std::string const& username, std::string const& prefix,
                                      std::string const& message, uint32 /*color*/)
{
    if (!channelId)
        return;

    DiscordConfig const* cfg = sDiscordMgr->Config();

    // Base content: emoji prefix + message.
    std::string base = prefix.empty() ? message : prefix + " " + message;
    // Named content carries the sender name inline (used when the webhook
    // username is hidden and for bot posts).
    std::string inlineName = "**" + username + "**: ";
    std::string named = prefix.empty() ? inlineName + message : prefix + " " + inlineName + message;

    // Prefer guild-owned emoji markup so webhook messages stay on the webhook
    // path whenever a same-named guild emoji exists.
    base = sDiscordMgr->Emojis()->RewriteAppEmojisToGuild(base);
    named = sDiscordMgr->Emojis()->RewriteAppEmojisToGuild(named);

    if (cfg->UseWebhooks && !webhookUrl.empty())
    {
        // Discord webhooks cannot render application emojis (only emojis from the
        // webhook's own guild). If the content uses app-owned emojis, post as the
        // bot instead so they render.
        if (sDiscordMgr->Emojis()->ContainsAppEmoji(cfg->HideWebhookName ? named : base))
        {
            DiscordJson::Writer w;
            w.Open();
            w.Field("content", named);
            w.Close();

            DiscordOutMessage msg;
            msg.payload = w.out;
            msg.path = "/channels/" + std::to_string(channelId) + "/messages";
            msg.method = "POST";
            msg.needsAuth = true;
            GetDiscordQuickSender().Send(std::move(msg));
            return;
        }

        // Webhook. With HideWebhookName the sender name is carried inline in the
        // message and we keep the webhook's configured default display name.
        // This lets manual webhooks show a stable label like "APP" without a
        // per-message username override.
        DiscordJson::Writer w;
        w.Open();
        w.Field("content", cfg->HideWebhookName ? named : base);
        if (!cfg->HideWebhookName)
            w.Field("username", username);
        w.Close();

        DiscordOutMessage msg;
        msg.payload = w.out;
        msg.path = webhookUrl;
        msg.method = "POST";
        msg.needsAuth = false;
        msg.isWebhook = true;
        GetDiscordQuickSender().Send(std::move(msg));
    }
    else
    {
        if (cfg->UseWebhooks)
        {
            LOG_WARN("modules.discord.bridge", "Chat send skipped because webhook mode is enabled but no "
                     "webhook URL is available for channel {}.", channelId);
            return;
        }

        // Bot message fallback: inline name + message. If webhooks were wanted but
        // no webhook URL is available, log it loudly so it is easy to diagnose.
        DiscordJson::Writer w;
        w.Open();
        w.Field("content", named);
        w.Close();

        DiscordOutMessage msg;
        msg.payload = w.out;
        msg.path = "/channels/" + std::to_string(channelId) + "/messages";
        msg.method = "POST";
        msg.needsAuth = true;
        GetDiscordQuickSender().Send(std::move(msg));
    }
}

void DiscordChatBridge::RelayToDiscord(Player* player, std::string const& message)
{
    if (!player || message.empty())
        return;

    DiscordConfig const* cfg = sDiscordMgr->Config();
    if (!cfg || !cfg->GlobalChatEnable)
        return;

    // Never relay bot chat unless explicitly enabled.
    if (sDiscordMgr->PlayerProvider()->IsPlayerBot(player) && !cfg->GlobalChatIncludeBots)
        return;

    // Deduplicate: the same player + message within a few seconds is a double
    // capture (e.g. command hook + channel hook) - post only once.
    uint32 now = uint32(GameTime::GetUptime().count());
    std::string dedupKey = player->GetName() + "\x01" + message;
    auto dedupIt = recentRelays_.find(dedupKey);
    if (dedupIt != recentRelays_.end() && now - dedupIt->second < 3)
        return;
    recentRelays_[dedupKey] = now;

    // Apply the chat filter (game -> Discord direction).
    std::string outMessage = message;
    if (sDiscordMgr->Filter()->AppliesToDiscord())
    {
        std::string filtered = sDiscordMgr->Filter()->Handle(message);
        if (filtered.empty())
        {
            LOG_INFO("modules.discord.bridge", "Blocked {} from relaying to Discord (filtered content).",
                     player->GetName());
            return;
        }
        outMessage = filtered;
    }

    // Never let in-game text ping Discord: strip @everyone / @here and mention
    // syntax before the message is posted.
    outMessage = StripDiscordMentions(outMessage);

    DiscordPlayerIdentity identity = sDiscordMgr->PlayerProvider()->GetIdentity(player);

    uint32 team = uint32(player->GetTeamId());
    uint64_t channelId = ResolveChatChannel(team);
    std::string webhookUrl = ResolveWebhookUrl(team);

    // No configured or cached webhook: create a transparent-avatar webhook for
    // this channel (invisible name + no icon), then keep using bot messages
    // until it is ready.
    if (sDiscordMgr->Config()->UseWebhooks && webhookUrl.empty())
        EnsureWebhook(channelId);

    std::string username = identity.valid ? identity.name : player->GetName();
    std::string prefix = sDiscordMgr->Emojis()->RenderPrefix(identity);

    // Blizz icon badge for Game Masters who have ".gm chat" enabled.
    if (player->IsGameMaster() && player->isGMChat())
    {
        std::string gm = sDiscordMgr->Emojis()->RenderGm();
        if (!gm.empty())
            prefix = prefix.empty() ? gm : gm + " " + prefix;
    }

    if (sDiscordMgr->Config()->UseWebhooks && webhookUrl.empty())
    {
        QueuePendingWebhookMessage(channelId, username, prefix, outMessage, DiscordChat::ClassColor(identity.cls));
        LOG_INFO("modules.discord.bridge", "Queued chat message for channel {} while webhook is being created.",
                 channelId);
        return;
    }

    SendToChannel(channelId, webhookUrl, username, prefix, outMessage, DiscordChat::ClassColor(identity.cls));
}

// ---- Game -> Discord capture -----------------------------------------------

void DiscordChatBridge::SetGlobalChatEnabled(uint32 playerGuid, bool enabled) const
{
    globalChatEnabled_[playerGuid] = enabled;
}

bool DiscordChatBridge::IsGlobalChatEnabled(uint32 playerGuid) const
{
    auto it = globalChatEnabled_.find(playerGuid);
    return it != globalChatEnabled_.end() && it->second;
}

void DiscordChatBridge::HandleGameCommand(Player* player, std::string_view cmdStr)
{
    DiscordConfig const* cfg = sDiscordMgr->Config();
    if (!cfg || !cfg->GlobalChatEnable || !cfg->RelayDirectGameMessages)
        return;

    auto provider = sGlobalChatProviderMgr->GetProvider();
    if (!provider)
        return;

    std::string cmdName = provider->GetCommandName();
    if (cmdName.empty())
        return;

    std::string cmd(cmdStr);
    size_t pos = cmd.find(' ');
    if (pos == std::string::npos)
        return; // bare command (e.g. ".chat" with no message / on / off)
    std::string first = cmd.substr(0, pos);
    std::string arg = cmd.substr(pos + 1);

    if (!StartsWithCI(first, cmdName))
        return;

    // mod-global-chat's on/off subcommands toggle visibility; not chat messages.
    std::string lowerArg = arg;
    for (char& c : lowerArg)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    if (lowerArg == "on")
    {
        SetGlobalChatEnabled(uint32(player->GetGUID().GetCounter()), true);
        return;
    }
    if (lowerArg == "off")
    {
        SetGlobalChatEnabled(uint32(player->GetGUID().GetCounter()), false);
        return;
    }

    if (arg.find_first_not_of(' ') == std::string::npos)
        return;

    // Only relay what would actually be broadcast in-game. If the player has not
    // enabled global chat (.chat on), or is muted, the provider will not send,
    // so we do not relay it to Discord either.
    if (!IsGlobalChatEnabled(uint32(player->GetGUID().GetCounter())))
        return;
    if (!player->CanSpeak())
        return;

    RelayToDiscord(player, arg);
}

void DiscordChatBridge::HandleChannelChat(Player* player, std::string const& channelName, std::string const& msg)
{
    DiscordConfig const* cfg = sDiscordMgr->Config();
    if (!cfg || !cfg->GlobalChatEnable || !cfg->RelayDirectGameMessages)
        return;

    auto provider = sGlobalChatProviderMgr->GetProvider();
    if (!provider || provider->GetChannelName().empty())
        return;

    if (channelName != provider->GetChannelName())
        return;

    if (msg.find_first_not_of(' ') == std::string::npos)
        return;

    // Only relay what would actually be broadcast in-game.
    if (!player->CanSpeak())
        return;

    RelayToDiscord(player, msg);
}

void DiscordChatBridge::HandleDiscordSay(Player* player, std::string const& msg)
{
    DiscordConfig const* cfg = sDiscordMgr->Config();
    if (!cfg || !cfg->GlobalChatEnable || !cfg->RelayDirectGameMessages)
        return;
    RelayToDiscord(player, msg);
}

// ---- Discord -> Game -------------------------------------------------------

std::string DiscordChatBridge::BuildGameMessage(DiscordChannelMessage const& msg) const
{
    DiscordConfig const* cfg = sDiscordMgr->Config();

    // Custom emojis typed by Discord users -> readable raw names.
    std::string text = ConvertCustomEmojisToNames(msg.content);

    // Remove @everyone / @here / <@mention> tags (meaningless in WoW chat).
    text = StripDiscordMentions(text);

    // Flatten replies: "â†³ @Name: original text"
    if (!msg.referencedAuthor.empty())
        text = "\xE2\x86\xB3 @" + msg.referencedAuthor + ": " + text;

    if (text.empty())
        return "";

    // Display name: guild nickname > username/global name.
    std::string speaker = msg.nickname.empty() ? msg.authorName : msg.nickname;
    if (speaker.empty())
        speaker = "Discord User";

    // Admin roles get the Blizz icon and a red name; a class role colors the name
    // with the class color; otherwise the name is yellow.
    DiscordMemberStyle style;
    if (cfg->RolesEnabled)
        style = sDiscordMgr->ResolveMemberStyle(msg.roles);

    // Faction + class icons from the configured role tags (instead of text tags).
    std::string factionIcon;
    std::string classIcon;
    uint8 classId = DiscordChat::INVALID_CLASS;
    sDiscordMgr->ResolveMemberIcons(msg.roles, factionIcon, classIcon, classId);

    std::string tag = cfg->DiscordTag;
    if (tag.empty())
        tag = "[Discord]";
    std::string tagColor = cfg->DiscordTagColor.size() == 6 ? cfg->DiscordTagColor : "5865F2";

    uint32 nameColor = style.isAdmin ? cfg->AdminColor
        : (classId != DiscordChat::INVALID_CLASS ? DiscordChat::ClassColor(classId) : 0xFFD100);

    std::ostringstream out;
    // Match the in-game Global Chat look: green "[Global]" tag first.
    out << "|cffABD473[Global] |r";
    out << "|cff" << tagColor << tag << "|r ";
    if (style.isAdmin)
        out << "|TINTERFACE/CHATFRAME/UI-CHATICON-BLIZZ:15|t ";
    if (!factionIcon.empty())
        out << factionIcon << " ";
    if (!classIcon.empty())
        out << classIcon << " ";
    out << "|cff" << Hex6(nameColor) << speaker << "|r: ";
    out << "|cffffffff" << text << "|r";
    return out.str();
}

void DiscordChatBridge::HandleDiscordToGame(std::string const& text, uint32 team)
{
    if (text.empty())
        return;
    auto provider = sGlobalChatProviderMgr->GetProvider();
    if (!provider)
        return;

    // For opt-in providers (.chat on), deliver only to players who enabled
    // global chat (matching the requested faction when split channels are used).
    if (provider->IsOptInOnly())
    {
        sWorldSessionMgr->DoForAllOnlinePlayers([&](Player* player) {
            if (team != TEAM_NEUTRAL && player->GetTeamId() != TeamId(team))
                return;
            if (!IsGlobalChatEnabled(uint32(player->GetGUID().GetCounter())))
                return;
            sWorldSessionMgr->SendServerMessage(SERVER_MSG_STRING, text, player);
        });
        return;
    }

    provider->SendToGame(text, TeamId(team));
}

void DiscordChatBridge::HandleMessageCreate(DiscordChannelMessage const& msg)
{
    DiscordConfig const* cfg = sDiscordMgr->Config();
    if (!initialized_ || !cfg || !cfg->GlobalChatEnable)
        return;

    // Ignore messages we posted ourselves (bot or webhook) and slash-command
    // responses (those are not chat and must never appear in world chat).
    if (msg.isInteraction || msg.authorIsBot || msg.fromWebhook)
        return;

    if (!cfg->IsChannelAllowedForChat(msg.channelId))
        return;

    // Messages that begin with "/" are Discord command attempts; never relay them.
    if (!msg.content.empty() && msg.content[0] == '/')
        return;

    // Attachments: never relay the attachment payloads by default. If the
    // message has only attachments and no text, skip it entirely.
    if (msg.hasAttachments && !cfg->Attachments && msg.content.empty())
        return;

    // Apply the chat filter (Discord -> game direction).
    DiscordChannelMessage filteredMsg = msg;
    if (cfg->FilterEnable && sDiscordMgr->Filter()->AppliesToGame())
    {
        std::string filtered = sDiscordMgr->Filter()->Handle(msg.content);
        if (filtered.empty())
        {
            LOG_INFO("modules.discord.bridge", "Blocked Discord message {} from reaching the game (filtered content).",
                     msg.authorId);
            return;
        }
        filteredMsg.content = filtered;
    }

    std::string gameText = BuildGameMessage(filteredMsg);
    if (gameText.empty())
        return;

    uint32 team = TEAM_NEUTRAL;
    if (cfg->SplitFactions)
    {
        if (msg.channelId == cfg->AllianceChannelId)
            team = TEAM_ALLIANCE;
        else if (msg.channelId == cfg->HordeChannelId)
            team = TEAM_HORDE;
    }

    HandleDiscordToGame(gameText, team);
}