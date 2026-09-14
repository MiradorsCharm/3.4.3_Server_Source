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
    /// add an existing character as a bot; masterGuid may be empty (random bot).
    /// The login is queued and released at a paced rate.
    bool AddBot(ObjectGuid guid, ObjectGuid masterGuid, bool random);
    /// same, but jump the login queue (".bot add" and a master's own login -
    /// a player waiting for their bot should not sit behind the pool)
    bool AddBotNow(ObjectGuid guid, ObjectGuid masterGuid, bool random);
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

    // --- persistence -----------------------------------------------------------
    /// remember a bot->master binding so the master's next login re-adds it
    void PersistBot(ObjectGuid botGuid, ObjectGuid masterGuid);
    /// forget the binding (".bot remove")
    void ForgetBot(ObjectGuid botGuid);
    /// re-add every persisted bot of this master (called from OnPlayerLogin)
    void LoadBotsForMaster(Player* master);
    /// write the state (roles, stay point, level, position is the core's own)
    /// of every online bot; runs periodically and on shutdown
    void SaveAllStates();

    // --- random bot pool ------------------------------------------------------
    void AuditRandomBots();               // keep ~N online
    uint32 GetRandomBotTarget() const;
    /// bots waiting for a login slot
    uint32 GetLoginQueueSize() const { return uint32(_loginQueue.size()); }

    /// per-world-tick update (called through the Playerbot OnWorldUpdate hook,
    /// world thread)
    void Update(uint32 diff);

private:
    BotManager() = default;

    void ProcessPendingLogins(uint32 diff);

    WorldSession* CreateBotSession(ObjectGuid guid);
    void DestroySession(ObjectGuid guid);

    void EnsureRandomBotPool();           // create accounts/characters when short
    std::vector<ObjectGuid> LoadPoolGuids();

    struct PendingLogin
    {
        ObjectGuid guid;
        ObjectGuid master;
        bool random = false;
        uint32 ageMs = 0;         // how long this login has been in flight
    };

    /// a bot waiting for a login slot
    struct QueuedLogin
    {
        ObjectGuid guid;
        ObjectGuid master;
        bool random = false;
    };

    /// hand one queued login to a session (called at a paced rate)
    void StartLogin(QueuedLogin const& entry);
    /// bring back the bots that were online when the server went down
    void RestoreRoster();

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

    // login pacing: a bot login loads a whole character, so releasing hundreds
    // of them in one tick is what used to freeze the world after every start
    std::deque<QueuedLogin> _loginQueue;
    uint32 _loginGateMs = 0;          // time until the next login may start
    uint32 _startupDelayMs = 0;       // grace period after boot
    bool _rosterRestored = false;

    uint32 _stateSaveTimer = 0;

    std::mutex _chatMutex;
    std::deque<QueuedChat> _chatQueue;

    uint32 _randomAuditTimer = 0;
    bool _shuttingDown = false;
};

#define sBotManager BotManager::instance()

#endif
