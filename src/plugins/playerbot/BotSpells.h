/*
 * Playerbot AI - spells.
 *
 * Thin, honest wrappers around the core's own spell plumbing. A bot casts
 * through Player::CastSpell exactly like the core's own charm code does; the
 * wrappers only decide WHETHER a cast makes sense (known, off cooldown,
 * affordable, in range, line of sight, alive/attackable) using the same
 * metrics the core's Spell::CheckCast will use a moment later.
 *
 * Auto-repeat: casting the wand-shoot (5019) or auto-shot (75) spell once
 * installs it in CURRENT_AUTOREPEAT_SPELL; Unit::_UpdateAutoRepeatSpell() then
 * keeps firing it every ranged swing while the bot stands still - the core
 * owns the loop, we only own the start.
 */

#ifndef PLAYERBOT_BOT_SPELLS_H
#define PLAYERBOT_BOT_SPELLS_H

#include "Define.h"
#include "ObjectGuid.h"
#include "SharedDefines.h"

#include <cstdint>
#include <vector>

class Player;
class SpellInfo;
class Unit;

class BotSpells
{
public:
    explicit BotSpells(Player* bot) : _bot(bot) { }

    bool Known(uint32 spellId) const;
    bool Ready(uint32 spellId) const;                 // known and not on cooldown (incl. GCD)
    bool Affordable(uint32 spellId) const;
    bool InRange(uint32 spellId, Unit* target) const;
    bool Castable(uint32 spellId, Unit* target) const; // the full gate, cheap
    bool Cast(uint32 spellId, Unit* target);           // gated cast
    bool CastSelf(uint32 spellId);

    /// highest rank of a spell family the bot knows (list ordered lowest->highest)
    uint32 HighestKnown(std::vector<uint32> const& rankIds) const;

    // --- auto-repeat (wand / auto shot) ------------------------------------
    bool IsAutoRepeating() const;
    void StartAutoRepeat(uint32 shootSpellId, Unit* target);
    void StopAutoRepeat();

    /// The ranged attack spell the bot's equipped ranged weapon grants:
    /// Auto Shot (75) for a hunter with a bow/gun/crossbow, Shoot (3018) for
    /// anyone else with one, Throw (2764) for thrown weapons, Shoot Wand
    /// (5019) for a wand, 0 with nothing equipped. This is the same rule the
    /// core's own PlayerAI uses (PlayerAI::DoRangedAttackIfReady) - a class
    /// table cannot answer it, because it is the *weapon* that decides.
    uint32 RangedAttackSpell() const;
    /// Keep the auto-repeat loop alive against a victim: (re)starts it when it
    /// is not running and nothing better is happening.
    void MaintainAutoRepeat(Unit* target);

    // --- state ---------------------------------------------------------------
    bool IsCasting() const;       // any spell being prepared/cast/channeled
    bool IsHardCasting() const;   // a spell with a cast time (bots must stand still)
    bool IsChanneling() const;
    void Interrupt();

    static bool IsEnemySpell(uint32 spellId); // helper for scripts (negative/positive target)

private:
    bool BasicTargetCheck(Unit* target) const;

    Player* _bot;
};

#endif
