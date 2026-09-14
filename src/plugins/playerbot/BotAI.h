/*
 * Playerbot AI - the per-bot brain.
 *
 * Attached to every logged-in bot (Player::GetPlayerbotAI) and driven from
 * Player::Update. One tick runs this pipeline:
 *
 *   1. teleport acks   (a bot has no client; the session pump synthesizes
 *                       them - see BotManager)
 *   2. death handling  (self-resurrect after the configured delay)
 *   3. movement        (the client-faithful mover, BotMovement)
 *   4. brain           (combat -> loot -> follow/stay/wander -> idle)
 *   5. diagnostics     (the stuck-bot watchdog)
 *
 * Everything the bot does goes through the same core paths a real player's
 * client would drive: attack state via Unit::Attack, spells via
 * Player::CastSpell, movement/facing via the bot's own WorldSession packets.
 */

#ifndef PLAYERBOT_BOT_AI_H
#define PLAYERBOT_BOT_AI_H

#include "BotMovement.h"
#include "BotSpells.h"
#include "BotCombat.h"
#include "BotLoot.h"
#include "BotClassAI.h"
#include "BotHazards.h"
#include "BotState.h"
#include "ObjectGuid.h"
#include "Optional.h"

#include <memory>
#include <string>
#include <vector>

class Player;
class WorldPacket;
class Unit;
class BotDiagnostics;

/// Where a bot sends its answers ("chat <mode>" command).
enum class BotChatChannel : uint8
{
    Whisper = 0,
    Party,
    Say
};

class BotAI
{
    friend class BotDiagnostics;

public:
    explicit BotAI(Player* bot);
    ~BotAI();

    /// called from Player::Update (map thread)
    void Update(uint32 diff);

    // --- accessors used by the sub-modules ---------------------------------
    Player* GetBot() const { return _bot; }
    BotMovement& GetMovement() { return *_movement; }
    BotSpells& GetSpells() { return *_spells; }
    BotCombat& GetCombat() { return *_combat; }
    BotLoot& GetLoot() { return *_loot; }
    BotClassAI& GetClassAI() { return *_classAI; }
    BotHazards& GetHazards() { return *_hazards; }

    // --- master / orders ---------------------------------------------------
    void SetMaster(Player* master);
    Player* GetMaster() const;
    ObjectGuid GetMasterGuid() const { return _masterGuid; }
    bool AcceptsCommandsFrom(Player* sender) const;

    // --- orders (chat commands) ---------------------------------------------
    /// master chat entry point: parses the whisper/party text and dispatches
    void HandleCommand(std::string const& msg, Player* sender);
    /// best-effort reply to the master
    void WhisperMaster(std::string const& text);
    /// reply on the channel the bot was told to use ("chat" command)
    void Reply(Player* to, std::string const& text);

    void CommandFollow();
    void CommandStay();
    void CommandCome(Player* sender);
    void CommandAttackMyTarget(Player* sender);
    void CommandAssist(Player* sender);
    void CommandStopAttack();
    void CommandLoot();
    void CommandRelease();
    void CommandStatus(Player* to);
    void CommandHeal();
    void CommandBuff();
    void CommandRez();
    void CommandCure();
    void CommandEat();
    void CommandDrink();
    void CommandUpgrade();
    void CommandRepair();
    void CommandTank();
    void CommandDps();
    void CommandGrind(bool on);
    void CommandSummon();
    void CommandQueue();
    void CommandLeave();
    void CommandGuild();
    void CommandGuildLeave();
    void CommandSell();
    void CommandQuests();

    // --- extended order set (adapted from the mangosbot command list) --------
    /// "follow far|near|melee|ranged" - how far behind the master we walk
    void CommandFollowMode(std::string const& mode);
    /// "formation far|near|melee|ranged" - same thing, mangosbot's wording
    void CommandFormation(std::string const& mode);
    /// "move out" - break the stack: everyone spreads a few yards apart
    void CommandMoveOut();
    /// "flee" - break off the current fight and run
    void CommandFlee();
    /// "tank attack my target" - take the sender's target as the tank
    void CommandTankAttack(Player* sender);
    /// "position" - report where we are
    void CommandPosition(Player* to);
    /// "who" - one line per bot in the group
    void CommandWho(Player* to);
    /// "stats" - numbers about this bot
    void CommandStats(Player* to);
    /// "spells" - the class abilities we actually know
    void CommandSpells(Player* to);
    /// "buy <name>" - purchase from the vendor we are standing at
    void CommandBuy(std::string const& what);
    /// "accept" / "accept all" - take the quests the sender is offering
    void CommandAcceptQuests();
    /// "cast <name>" - fire a named spell at the sender's target (or self)
    void CommandCastNamed(std::string const& name, Player* sender);
    /// "invite" - invite the sender into our group
    void CommandInvite(Player* sender);
    /// "emote <name>" - play an emote
    void CommandEmote(std::string const& name);
    /// "save mana" / "max dps" - how hard the rotation pushes
    void CommandSaveMana(bool on);
    /// "chat say|party|whisper" - where answers go
    void CommandChat(std::string const& mode);
    /// "reset ai" - drop every order and start over
    void CommandResetAI();

    // --- victim management (used by combat + retaliate) ----------------------
    void Attack(Unit* target, std::string reason);
    void StopAttacking(std::string reason);
    /// drop the fight and run away from a unit we cannot hurt
    void FleeFrom(Unit* attacker, std::string reason);
    /// called by BotCombat when our victim died (queue loot, drop combat)
    void OnVictimDied(Unit* victim);

    // --- party services (heals/rez/cure/buff, tank & grind modes) ------------
    /// master/group members (+pets) in range - the roster every party pass uses
    std::vector<Unit*> GetPartyUnits(float maxDist, bool includePets = true) const;
    /// dead (but not ghost) party member within range, lowest by distance
    Unit* FindDeadPartyMember(float range) const;
    /// party member with a dispellable debuff of the given mask
    Unit* FindDispelTarget(uint32 dispelMask, float range) const;
    /// do we carry food (drink=false) / water (drink=true)?
    bool HasConsumable(bool drink) const;
    /// eat/drink the best matching consumable; true when something was used
    bool TryConsume(bool drink);

    bool IsTankMode() const { return _tankMode; }
    void SetTankMode(bool on) { _tankMode = on; }
    bool CanTank() const;
    bool IsGrinding() const { return _grindMode; }
    void SetGrind(bool on) { _grindMode = on; }

    /// "save mana" holds the rotation back while resources are low;
    /// "max dps" spends freely. Both are orders, both are persisted.
    bool IsConservingMana() const { return _saveMana; }
    bool IsMaxDps() const { return _maxDps; }

    /// how far behind the master this bot walks ("follow far/near/...")
    float GetFollowDistance() const { return _followDistance; }
    BotChatChannel GetChatChannel() const { return _chatChannel; }

    // --- persistent state (see BotState) ------------------------------------
    /// take the flags of a restored bot back on (roles, stay point, orders)
    void ApplySavedState(BotSavedState const& state);
    /// snapshot the flags worth keeping across a restart
    BotSavedState BuildState() const;
    void SaveState();

    // --- server -> bot packets (loot responses etc.) --------------------------
    void HandleBotOutgoingPacket(WorldPacket const* packet);

    // --- misc state ----------------------------------------------------------
    bool IsRandomBot() const { return _randomBot; }
    void SetRandomBot(bool random) { _randomBot = random; }

    uint32 GetStallSeconds() const { return _stallSeconds; }
    std::string const& GetLastOrder() const { return _lastOrder; }
    void SetLastOrder(std::string order) { _lastOrder = std::move(order); }

    /// stall bookkeeping shared with BotDiagnostics
    struct StallTracker
    {
        float lastDistance = 0.0f;
        uint32 lastVictimHealth = 0;
        bool hasMark = false;
    };
    StallTracker& GetStallTracker() { return _stallTracker; }

private:
    void UpdateDeath(uint32 diff);
    void UpdateBrain(uint32 diff);
    /// step out of ground effects (dungeon/raid mechanic awareness); returns
    /// true when it took control of movement this tick
    bool UpdateHazardAvoidance(uint32 diff);
    void UpdateRetaliate();
    /// run away from a fight we cannot win; true while it owns the tick
    bool UpdateFlee(uint32 diff);
    void UpdatePartyCare(uint32 diff);
    void UpdateConsume(uint32 diff);
    void UpdateGrind(uint32 diff);
    void UpdateNonCombat(uint32 diff);
    void UpdateWander(uint32 diff);
    void UpdateDiagnostics(uint32 diff);

    Unit* Resolve(Unit* who) const;
    bool CanSee(Unit* who) const;

    Player* _bot;
    std::unique_ptr<BotMovement> _movement;
    std::unique_ptr<BotSpells> _spells;
    std::unique_ptr<BotCombat> _combat;
    std::unique_ptr<BotLoot> _loot;
    std::unique_ptr<BotClassAI> _classAI;
    std::unique_ptr<BotHazards> _hazards;

    ObjectGuid _masterGuid;
    ObjectGuid _lastSender;              // whoever gave the last order (answers go there)
    bool _randomBot = false;
    std::string _lastOrder;

    // stay mode
    bool _stay = false;
    Optional<Position> _stayPoint;

    // roles
    bool _tankMode = false;
    bool _grindMode = false;

    // orders with persistent state
    bool _saveMana = false;
    bool _maxDps = false;
    float _followDistance = 0.0f;        // 0 = use AiPlayerbot.FollowDistance
    BotChatChannel _chatChannel = BotChatChannel::Whisper;

    // running away from a fight we cannot win
    ObjectGuid _fleeTarget;
    uint32 _fleeTimer = 0;
    uint32 _fleeCooldown = 0;
    uint32 _relocateTimer = 0;           // random bot roaming

    // party care / consume / grind throttles
    uint32 _partyCareCooldown = 0;
    uint32 _consumeCooldown = 0;
    uint32 _grindScanCooldown = 0;
    uint32 _forceConsumeTimer = 0;
    uint32 _masterTeleportCooldown = 0;    // catch-up teleport pacing
    uint32 _talentTimer = 0;               // periodic talent point spending
    uint32 _questTimer = 0;                // periodic quest turn-in sweep

    // death / revive
    uint32 _deadTimer = 0;

    // dungeon/raid mechanic awareness
    bool _dodging = false;                 // currently running out of a hazard
    uint32 _hazardReactCooldown = 0;       // pace re-issuing a new escape goal
    uint32 _interruptCooldown = 0;         // pace boss-cast interrupt attempts

    // idle timers
    uint32 _regenCheckTimer = 0;
    uint32 _wanderTimer = 0;
    uint32 _diagnosticsTimer = 0;
    uint32 _stallSeconds = 0;
    uint32 _stallReportCooldown = 0;
    StallTracker _stallTracker;
};

#endif
