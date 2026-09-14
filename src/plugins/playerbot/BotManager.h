/*
 * Playerbot AI - manager.
 *
 * Owns every bot's socket-less WorldSession (never part of the world session
 * map), pumps them once per world tick, attaches the AI when a bot finishes
 * logging in, and runs the random bot pool. All session/container mutations
 * happen on the world thread (World::Update -> OnWorldUpdate, chat/command
 * handlers); per-bot AI state lives in BotAI and is touched only by that
 * bot's own Player::Update on the map thread.
 */

#ifndef PLAYERBOT_BOT_MANAGER_H
#define PLAYERBOT_BOT_MANAGER_H

#include "Define.h"
#include "ObjectGuid.h"

#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class Player;
class WorldSession;
class BotAI;
struct ChatHandler;

class BotManager
{
public:
    static BotManager* instance()
    {
        static BotManager mgr;
        return &mgr;
    }

    /// fills the Playerbot hooks + loads config (called from worldserver Main)
    bool Initialize();
    void Shutdown();

    // --- bot lifecycle -------------------------------------------------------
    /// add an existing character as a bot; masterGuid may be empty (random bot)
    bool AddBot(ObjectGuid guid, ObjectGuid masterGuid, bool random);
    /// logout a bot (by character guid or name)
    bool RemoveBot(std::string const& name);
    void RemoveBot(ObjectGuid guid);
    void RemoveAllBots();

    Player* GetBot(ObjectGuid guid) const;
    Player* GetBotByName(std::string const& name) const;
    BotAI* GetAI(Player* bot) const;
    BotAI* GetAIByName(std::string const& name) const;

    std::vector<Player*> GetAllBots() const;

    /// whisper routing (called from the core chat hook; queued for the world
    /// thread - chat handlers run on map threads)
    void HandleChat(Player* sender, uint32 type, uint32 lang, std::string const& msg, Player* receiver);
    /// group invites: bots accept invites from their master automatically
    void HandleGroupInviteChanged(Player* bot);

    // --- random bot pool ------------------------------------------------------
    void AuditRandomBots();               // keep ~N online
    uint32 GetRandomBotTarget() const;

    /// per-world-tick update (called through the Playerbot OnWorldUpdate hook,
    /// world thread)
    void Update(uint32 diff);

private:
    BotManager() = default;

    void ProcessPendingLogins();

    WorldSession* CreateBotSession(ObjectGuid guid);
    void DestroySession(ObjectGuid guid);

    void EnsureRandomBotPool();           // create accounts/characters when short
    std::vector<ObjectGuid> LoadPoolGuids();

    struct PendingLogin
    {
        ObjectGuid guid;
        ObjectGuid master;
        bool random = false;
    };

    /// queued chat command (routed on the world thread)
    struct QueuedChat
    {
        ObjectGuid sender;
        ObjectGuid receiver;    // empty for party/raid routing
        uint32 type = 0;
        uint32 lang = 0;
        std::string msg;
    };

    void RouteChat(QueuedChat const& chat);

    std::unordered_map<ObjectGuid, std::unique_ptr<WorldSession>> _sessions;
    std::unordered_map<ObjectGuid, PendingLogin> _pending;
    std::vector<ObjectGuid> _logoutQueue;

    std::mutex _chatMutex;
    std::deque<QueuedChat> _chatQueue;

    uint32 _randomAuditTimer = 0;
    bool _shuttingDown = false;
};

#define sBotManager BotManager::instance()

#endif
