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
#include "ObjectGuid.h"
#include "Optional.h"

#include <memory>
#include <string>
#include <vector>

class Player;
class WorldPacket;
class Unit;
class BotDiagnostics;

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

    // --- victim management (used by combat + retaliate) ----------------------
    void Attack(Unit* target, std::string reason);
    void StopAttacking(std::string reason);
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
    void UpdateRetaliate();
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

    ObjectGuid _masterGuid;
    bool _randomBot = false;
    std::string _lastOrder;

    // stay mode
    bool _stay = false;
    Optional<Position> _stayPoint;

    // roles
    bool _tankMode = false;
    bool _grindMode = false;

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

    // idle timers
    uint32 _regenCheckTimer = 0;
    uint32 _wanderTimer = 0;
    uint32 _diagnosticsTimer = 0;
    uint32 _stallSeconds = 0;
    uint32 _stallReportCooldown = 0;
    StallTracker _stallTracker;
};

#endif
