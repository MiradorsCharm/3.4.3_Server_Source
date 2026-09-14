/*
 * Playerbot AI - combat.
 *
 * One module owns "the bot and its victim". It reproduces, deliberately
 * simply, what a player does when told to fight something:
 *
 *   melee:   run at the victim until the CORE's own swing gate
 *            (IsWithinMeleeRange + HasInArc, the exact checks
 *            Unit::DoMeleeAttackIfReady performs) passes, then stop, face,
 *            and let Player::Update swing while the class script spends
 *            resources on top.
 *   ranged:  hold a band between the weapon/spell minimum range and the
 *            configured stand-off, face the victim, keep the auto-repeat
 *            wand/auto-shot loop alive and let the class script cast.
 *
 * Every decision reads the same numbers the core will use one tick later -
 * there is no second source of truth for range/facing anywhere in the bot.
 */

#ifndef PLAYERBOT_BOT_COMBAT_H
#define PLAYERBOT_BOT_COMBAT_H

#include "Define.h"
#include "ObjectGuid.h"

#include <string>

class Player;
class Unit;
class BotAI;

enum class BotCombatStance
{
    Melee,      // walk into swing range and swing
    Ranged      // hold a distance band and shoot/cast
};

class BotCombat
{
public:
    BotCombat(BotAI* ai, Player* bot) : _ai(ai), _bot(bot) { }

    void Update(uint32 diff);

    void SetVictim(Unit* victim, std::string reason);
    void ClearVictim(std::string reason);
    Unit* GetVictim() const;
    bool HasVictim() const { return !_victimGuid.IsEmpty(); }
    std::string const& GetVictimReason() const { return _victimReason; }

    void SetStance(BotCombatStance stance) { _stance = stance; }
    BotCombatStance GetStance() const { return _stance; }

    /// is this unit still a fightable victim for our bot?
    bool IsValidVictim(Unit* victim) const;

    /// Retreat (kite) cooldown helpers used by the ranged stance.
    bool CanRetreat() const { return _retreatCooldown == 0; }
    void StartedRetreat() { _retreatCooldown = 2000; }

    /// true if we dealt with the "start the auto attack" bookkeeping already
    bool AttackStartedFor(Unit* victim) const;

private:
    void UpdateMelee(Unit* victim, uint32 diff);
    void UpdateRanged(Unit* victim, uint32 diff);

    BotAI* _ai;
    Player* _bot;

    ObjectGuid _victimGuid;
    std::string _victimReason;
    BotCombatStance _stance = BotCombatStance::Melee;

    ObjectGuid _attackStartedGuid;  // Attack() already called for this victim
    uint32 _retreatCooldown = 0;
    uint32 _castPaceCooldown = 0;   // one class-script rotation attempt per GCD-ish window
};

#endif
