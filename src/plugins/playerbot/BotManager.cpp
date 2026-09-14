#include "BotManager.h"

#include "BotAI.h"
#include "BotConfig.h"
#include "BotFactory.h"
#include "Player.h"
#include "WorldSession.h"
#include "Server/Packets/MovementPackets.h"
#include "ObjectAccessor.h"
#include "CharacterCache.h"
#include "AccountMgr.h"
#include "DatabaseEnv.h"
#include "World.h"
#include "Chat/Chat.h"
#include "Log.h"
#include "Util.h"
#include "Playerbot/PlayerbotHooks.h"

#include <algorithm>
#include <cctype>

namespace
{
    bool IEquals(std::string const& a, std::string const& b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    }
}


// ---------------------------------------------------------------------------
// session handling
// ---------------------------------------------------------------------------

WorldSession* BotManager::CreateBotSession(ObjectGuid guid)
{
    uint32 accountId = sCharacterCache->GetCharacterAccountIdByGuid(guid);
    if (!accountId)
    {
        TC_LOG_ERROR("playerbot", "Cannot resolve the account of bot {}", guid.ToString());
        return nullptr;
    }

    std::string accountName;
    if (!AccountMgr::GetName(accountId, accountName))
        accountName = "playerbot";

    auto session = std::make_unique<WorldSession>(accountId, std::move(accountName), 0, nullptr, SEC_PLAYER,
        uint8(sWorld->getIntConfig(CONFIG_EXPANSION)), 0, "", Minutes(0), LOCALE_enUS, 0, false);
    session->SetBotSession(true);
    session->LoginBotPlayer(guid);

    WorldSession* raw = session.get();
    _sessions[guid] = std::move(session);
    return raw;
}

void BotManager::DestroySession(ObjectGuid guid)
{
    auto it = _sessions.find(guid);
    if (it == _sessions.end())
        return;

    WorldSession* session = it->second.get();
    if (Player* bot = session->GetPlayer())
    {
        if (BotAI* ai = bot->GetBotAI())
        {
            bot->SetBotAI(nullptr);
            delete ai;
        }
        TC_LOG_INFO("playerbot", "Bot {} logged out", bot->GetName());
        session->LogoutPlayer(true);
    }
    _sessions.erase(it);
}

bool BotManager::AddBot(ObjectGuid guid, ObjectGuid masterGuid, bool random)
{
    if (!sBotConfig->Enabled || !guid.IsPlayer())
        return false;

    if (_sessions.count(guid) || _pending.count(guid))
        return true;    // already online or on the way

    // must not be online with a real client
    if (Player* existing = ObjectAccessor::FindConnectedPlayer(guid))
    {
        if (!existing->GetBotAI())
        {
            TC_LOG_ERROR("playerbot", "Bot {} is already online", guid.ToString());
            return false;
        }
        return true;
    }

    CharacterCacheEntry const* cache = sCharacterCache->GetCharacterCacheByGuid(guid);
    if (!cache || cache->IsDeleted)
    {
        TC_LOG_ERROR("playerbot", "Cannot add bot {}: character not found", guid.ToString());
        return false;
    }

    PendingLogin pending;
    pending.guid = guid;
    pending.master = masterGuid;
    pending.random = random;
    _pending[guid] = pending;

    CreateBotSession(guid);
    return true;
}

void BotManager::ProcessPendingLogins()
{
    if (_pending.empty())
        return;

    std::vector<ObjectGuid> done;
    for (auto const& [guid, pending] : _pending)
    {
        auto it = _sessions.find(guid);
        if (it == _sessions.end())
        {
            done.push_back(guid);
            continue;
        }

        WorldSession* session = it->second.get();
        session->HandleBotPackets();

        Player* bot = session->GetPlayer();
        if (bot && bot->IsBeingTeleportedFar())
        {
            // a bot has no client to send the worldport ack; do it directly
            session->HandleMoveWorldportAck();
            bot = session->GetPlayer();
        }

        if (bot && bot->IsInWorld())
        {
            BotAI* ai = new BotAI(bot);
            bot->SetBotAI(ai);
            ai->SetRandomBot(pending.random);

            // Gear/spell prep first (it saves the character), then the
            // summon - otherwise the save races the pending teleport.
            BotFactory::PrepareBot(bot);

            if (!pending.master.IsEmpty())
            {
                if (Player* master = ObjectAccessor::FindConnectedPlayer(pending.master))
                {
                    ai->SetMaster(master);

                    // A bot summoned from across the world walks for hours;
                    // drop it next to its master instead (only when it is not
                    // already close). The teleport's acks are synthesized by
                    // the session pump below.
                    if (master->GetMapId() != bot->GetMapId() || bot->GetExactDist(master) > 100.0f)
                        bot->TeleportTo(master->GetMapId(), master->GetPositionX(), master->GetPositionY(),
                            master->GetPositionZ(), master->GetOrientation());
                }
            }

            TC_LOG_INFO("playerbot", "Bot {} entered the world{}",
                bot->GetName(), pending.random ? " (random)" : "");
            done.push_back(guid);
        }
    }

    for (ObjectGuid guid : done)
        _pending.erase(guid);
}

void BotManager::RemoveBot(ObjectGuid guid)
{
    _pending.erase(guid);
    _logoutQueue.push_back(guid);
}

bool BotManager::RemoveBot(std::string const& name)
{
    Player* bot = GetBotByName(name);
    if (!bot)
        return false;
    RemoveBot(bot->GetGUID());
    return true;
}

void BotManager::RemoveAllBots()
{
    for (auto const& [guid, session] : _sessions)
        _logoutQueue.push_back(guid);
}

// ---------------------------------------------------------------------------
// lookups
// ---------------------------------------------------------------------------

Player* BotManager::GetBot(ObjectGuid guid) const
{
    auto it = _sessions.find(guid);
    if (it == _sessions.end())
        return nullptr;
    Player* bot = it->second->GetPlayer();
    return bot && bot->GetBotAI() ? bot : nullptr;
}

Player* BotManager::GetBotByName(std::string const& name) const
{
    for (auto const& [guid, session] : _sessions)
        if (Player* bot = session->GetPlayer())
            if (IEquals(bot->GetName(), name))
                return bot;
    return nullptr;
}

BotAI* BotManager::GetAI(Player* bot) const
{
    return bot ? bot->GetBotAI() : nullptr;
}

BotAI* BotManager::GetAIByName(std::string const& name) const
{
    Player* bot = GetBotByName(name);
    return bot ? bot->GetBotAI() : nullptr;
}

std::vector<Player*> BotManager::GetAllBots() const
{
    std::vector<Player*> bots;
    for (auto const& [guid, session] : _sessions)
        if (Player* bot = session->GetPlayer())
            if (bot->GetBotAI())
                bots.push_back(bot);
    return bots;
}

// ---------------------------------------------------------------------------
// random bot pool
// ---------------------------------------------------------------------------

std::vector<ObjectGuid> BotManager::LoadPoolGuids()
{
    std::vector<ObjectGuid> pool;

    // all characters whose account name starts with the bot prefix
    std::string prefix = sBotConfig->RandomBotAccountPrefix + "%";
    QueryResult result = LoginDatabase.PQuery(
        "SELECT a.id FROM account a WHERE a.username LIKE '{}'", prefix);
    if (result)
    {
        do
        {
            uint32 accountId = result->Fetch()[0].GetUInt32();

            QueryResult chars = CharacterDatabase.PQuery(
                "SELECT guid FROM characters WHERE account = {} AND deleteInfos_Name IS NULL", accountId);
            if (chars)
                do
                {
                    ObjectGuid::LowType low = chars->Fetch()[0].GetUInt64();
                    pool.push_back(ObjectGuid::Create<HighGuid::Player>(low));
                } while (chars->NextRow());
        } while (result->NextRow());
    }

    return pool;
}

uint32 BotManager::GetRandomBotTarget() const
{
    return sBotConfig->RandomBotCount;
}

namespace
{
    // a valid race per class (WotLK races, DK excluded from the random pool)
    uint8 PickRaceForClass(uint8 cls)
    {
        static uint8 const races[][10] =
        {
            // human, dwarf, night elf, gnome, draenei, orc, undead, tauren, troll, blood elf
            { 0 },
        };
        (void)races;

        switch (cls)
        {
            case CLASS_WARRIOR: return urand(0, 1) ? RACE_HUMAN : RACE_ORC;
            case CLASS_PALADIN: return urand(0, 1) ? RACE_HUMAN : RACE_BLOODELF;
            case CLASS_HUNTER: return urand(0, 1) ? RACE_DWARF : RACE_TROLL;
            case CLASS_ROGUE: return urand(0, 1) ? RACE_HUMAN : RACE_UNDEAD_PLAYER;
            case CLASS_PRIEST: return urand(0, 1) ? RACE_HUMAN : RACE_TROLL;
            case CLASS_SHAMAN: return urand(0, 1) ? RACE_DRAENEI : RACE_TAUREN;
            case CLASS_MAGE: return urand(0, 1) ? RACE_GNOME : RACE_BLOODELF;
            case CLASS_WARLOCK: return urand(0, 1) ? RACE_GNOME : RACE_ORC;
            case CLASS_DRUID: return urand(0, 1) ? RACE_NIGHTELF : RACE_TAUREN;
            default: return RACE_HUMAN;
        }
    }
}

void BotManager::EnsureRandomBotPool()
{
    std::vector<ObjectGuid> pool = LoadPoolGuids();
    uint32 const target = GetRandomBotTarget();
    if (pool.size() >= target)
        return;

    // create accounts+characters until the pool is big enough (best effort,
    // a few per audit so a cold start does not hammer the database)
    uint32 const need = std::min<uint32>(target - uint32(pool.size()), 5);
    uint32 const classCount = 9; // warrior..druid (DKs are excluded from random pools)
    time_t const now = time(nullptr);

    for (uint32 i = 0; i < need; ++i)
    {
        std::string accountName = sBotConfig->RandomBotAccountPrefix + "_" + std::to_string(uint64(now) + i);
        if (sAccountMgr->CreateAccount(accountName, "playerbot") != AccountOpResult::AOR_OK)
        {
            TC_LOG_ERROR("playerbot", "Could not create bot account {}", accountName);
            return;
        }

        uint32 accountId = 0;
        if (QueryResult result = LoginDatabase.PQuery("SELECT id FROM account WHERE username = '{}'", accountName))
            accountId = result->Fetch()[0].GetUInt32();
        if (!accountId)
            return;

        std::string name;
        if (QueryResult result = CharacterDatabase.Query(
            "SELECT name FROM ai_playerbot_names WHERE name NOT IN (SELECT name FROM characters) ORDER BY RAND() LIMIT 1"))
            name = result->Fetch()[0].GetString();
        else
            name = "Bot" + std::to_string(uint64(now) % 100000 + i);

        uint8 const cls = uint8(urand(1, classCount));
        uint8 const race = PickRaceForClass(cls);

        if (!BotFactory::CreateBotCharacter(accountId, name, cls, race, uint8(urand(0, 1))))
            continue;
    }
}

void BotManager::AuditRandomBots()
{
    if (GetRandomBotTarget() == 0)
    {
        // feature off: log every random bot out
        for (auto const& [guid, session] : _sessions)
            if (Player* bot = session->GetPlayer())
                if (BotAI* ai = bot->GetBotAI())
                    if (ai->IsRandomBot())
                        _logoutQueue.push_back(guid);
        return;
    }

    std::vector<Player*> randomOnline;
    for (auto const& [guid, session] : _sessions)
        if (Player* bot = session->GetPlayer())
            if (BotAI* ai = bot->GetBotAI())
                if (ai->IsRandomBot())
                    randomOnline.push_back(bot);

    uint32 const target = GetRandomBotTarget();

    // too many: log the rest out
    while (randomOnline.size() > target)
    {
        Player* victim = randomOnline.back();
        randomOnline.pop_back();
        _logoutQueue.push_back(victim->GetGUID());
    }

    if (randomOnline.size() < target)
    {
        EnsureRandomBotPool();

        std::vector<ObjectGuid> pool = LoadPoolGuids();
        // shuffle for a natural-looking selection
        for (size_t i = pool.size(); i > 1; --i)
            std::swap(pool[i - 1], pool[urand(0, uint32(i) - 1)]);

        // pending logins count toward the target; never queue more than the
        // remaining gap (and never put placeholder entries in randomOnline -
        // the trim above would dereference them on the next audit)
        uint32 online = uint32(randomOnline.size());
        for (ObjectGuid guid : pool)
        {
            if (online >= target)
                break;
            if (_sessions.count(guid) || _pending.count(guid))
                continue;
            if (!AddBot(guid, ObjectGuid::Empty, true))
                continue;
            ++online;
        }
    }
}

// ---------------------------------------------------------------------------
// per-tick update (world thread)
// ---------------------------------------------------------------------------

void BotManager::Update(uint32 diff)
{
    if (_shuttingDown)
        return;

    // drain queued chat commands (world thread - safe against the container)
    {
        std::deque<QueuedChat> chats;
        {
            std::lock_guard<std::mutex> lock(_chatMutex);
            chats.swap(_chatQueue);
        }
        for (QueuedChat const& chat : chats)
            RouteChat(chat);
    }

    // pending logins first: they become bots within a tick or two
    ProcessPendingLogins();

    // pump every live session
    std::vector<ObjectGuid> sessions;
    sessions.reserve(_sessions.size());
    for (auto const& [guid, session] : _sessions)
        sessions.push_back(guid);

    for (ObjectGuid guid : sessions)
    {
        auto it = _sessions.find(guid);
        if (it == _sessions.end())
            continue;

        // pending logins were just pumped in ProcessPendingLogins
        if (_pending.count(guid))
            continue;

        WorldSession* session = it->second.get();
        Player* bot = session->GetPlayer();

        if (bot && bot->GetBotAI())
        {
            if (bot->IsBeingTeleportedFar())
                session->HandleMoveWorldportAck();
            else if (bot->IsBeingTeleportedNear())
            {
                // synthesize the teleport ack (no client to do it)
                WorldPackets::Movement::MoveTeleportAck ack{ WorldPacket(CMSG_MOVE_TELEPORT_ACK) };
                ack.MoverGUID = bot->GetGUID();
                session->HandleMoveTeleportAck(ack);
            }

            if (bot && bot->IsInWorld())
                session->HandleBotPackets();
            continue;
        }

        if (!bot)
        {
            // still loading
            session->HandleBotPackets();
            if (!session->GetPlayer() && !session->PlayerLoading())
            {
                // login failed
                TC_LOG_ERROR("playerbot", "Bot {} failed to enter the world", guid.ToString());
                _sessions.erase(it);
            }
            continue;
        }

        // player present but no AI: shouldn't happen; clean up
        _logoutQueue.push_back(guid);
    }

    // queued logouts
    while (!_logoutQueue.empty())
    {
        ObjectGuid guid = _logoutQueue.back();
        _logoutQueue.pop_back();
        DestroySession(guid);
    }

    // random bot pool audit
    _randomAuditTimer += diff;
    if (_randomAuditTimer >= sBotConfig->RandomBotUpdateInterval * IN_MILLISECONDS)
    {
        _randomAuditTimer = 0;
        AuditRandomBots();
    }
}

// ---------------------------------------------------------------------------
// chat
// ---------------------------------------------------------------------------

void BotManager::HandleChat(Player* sender, uint32 type, uint32 lang, std::string const& msg, Player* receiver)
{
    if (!sender || lang == LANG_ADDON || lang == LANG_ADDON_LOGGED)
        return;

    if (type != CHAT_MSG_WHISPER && type != CHAT_MSG_PARTY && type != CHAT_MSG_RAID && type != CHAT_MSG_SAY)
        return;

    // Chat handlers run on the sender's map thread while this manager lives
    // on the world thread: queue the message here and route it from Update().
    QueuedChat chat;
    chat.sender = sender->GetGUID();
    chat.receiver = receiver ? receiver->GetGUID() : ObjectGuid::Empty;
    chat.type = type;
    chat.lang = lang;
    chat.msg = msg;

    std::lock_guard<std::mutex> lock(_chatMutex);
    if (_chatQueue.size() < 128)    // bounded: a chat flood is not a memory leak
        _chatQueue.push_back(std::move(chat));
}

void BotManager::RouteChat(QueuedChat const& chat)
{
    Player* sender = ObjectAccessor::FindConnectedPlayer(chat.sender);
    if (!sender)
        return;

    if (chat.type == CHAT_MSG_WHISPER)
    {
        if (Player* receiver = ObjectAccessor::FindConnectedPlayer(chat.receiver))
            if (BotAI* ai = receiver->GetBotAI())
                ai->HandleCommand(chat.msg, sender);
        return;
    }

    // party/raid/say commands: route to bots whose master is the sender
    for (Player* bot : GetAllBots())
    {
        BotAI* ai = bot->GetBotAI();
        if (!ai)
            continue;
        if (Player* master = ai->GetMaster())
            if (master == sender)
                ai->HandleCommand(chat.msg, sender);
    }
}

// ---------------------------------------------------------------------------
// init / shutdown
// ---------------------------------------------------------------------------

namespace
{
    void HookWorldUpdate(uint32 diff)
    {
        try
        {
            sBotManager->Update(diff);
        }
        catch (std::exception const& e)
        {
            TC_LOG_ERROR("playerbot", "Bot manager update failed: {}", e.what());
        }
        catch (...)
        {
            TC_LOG_ERROR("playerbot", "Bot manager update failed with an unknown exception");
        }
    }

    void HookPlayerUpdate(Player* player, uint32 diff)
    {
        if (BotAI* ai = player->GetBotAI())
        {
            try
            {
                ai->Update(diff);
            }
            catch (std::exception const& e)
            {
                TC_LOG_ERROR("playerbot", "Bot AI update for {} failed: {}", player->GetName(), e.what());
            }
            catch (...)
            {
                TC_LOG_ERROR("playerbot", "Bot AI update for {} failed with an unknown exception", player->GetName());
            }
        }
    }

    void HookBotPacketSent(Player* bot, WorldPacket const* packet)
    {
        if (BotAI* ai = bot->GetBotAI())
            ai->HandleBotOutgoingPacket(packet);
    }

    void HookPlayerChat(Player* sender, uint32 type, uint32 lang, std::string const& msg, Player* receiver)
    {
        try
        {
            sBotManager->HandleChat(sender, type, lang, msg, receiver);
        }
        catch (std::exception const& e)
        {
            TC_LOG_ERROR("playerbot", "Bot chat handling failed: {}", e.what());
        }
        catch (...)
        {
            TC_LOG_ERROR("playerbot", "Bot chat handling failed with an unknown exception");
        }
    }

    void HookPlayerDelete(Player* player)
    {
        // Player is going away (possibly on a map thread): detach the AI so
        // nothing dangles, and let the world-thread pump drop the session.
        if (WorldSession* session = player->GetSession())
            if (session->IsBotSession())
                session->SetPlayer(nullptr);

        if (BotAI* ai = player->GetBotAI())
        {
            player->SetBotAI(nullptr);
            delete ai;
        }
    }
}

bool BotManager::Initialize()
{
    if (!sBotConfig->Load())
        return false;

    Playerbot::Hooks& hooks = Playerbot::GetHooks();
    hooks.OnWorldUpdate = &HookWorldUpdate;
    hooks.OnPlayerUpdate = &HookPlayerUpdate;
    hooks.OnBotPacketSent = &HookBotPacketSent;
    hooks.OnPlayerChat = &HookPlayerChat;
    hooks.OnPlayerDelete = &HookPlayerDelete;
    Playerbot::SetEnabled(true);

    // ensure the names table exists so random bot creation works out of the box
    CharacterDatabase.Execute(
        "CREATE TABLE IF NOT EXISTS ai_playerbot_names ("
        "name_id INT UNSIGNED NOT NULL AUTO_INCREMENT,"
        "name VARCHAR(12) NOT NULL,"
        "gender TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "PRIMARY KEY(name_id), UNIQUE KEY idx_name(name)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");

    TC_LOG_INFO("playerbot", "Playerbot AI initialized");
    return true;
}

void BotManager::Shutdown()
{
    _shuttingDown = true;
    Playerbot::SetEnabled(false);
    for (auto const& [guid, session] : _sessions)
        _logoutQueue.push_back(guid);
    while (!_logoutQueue.empty())
    {
        ObjectGuid guid = _logoutQueue.back();
        _logoutQueue.pop_back();
        DestroySession(guid);
    }
}
