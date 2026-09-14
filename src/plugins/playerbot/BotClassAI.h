/*
 * Playerbot AI - class scripts.
 *
 * Each class gets one small, explicit script: a CombatTick() that spends
 * GCDs/resources in a priority order written by hand (openers, upkeep, burst,
 * fillers), a HealTick() used by the self-preservation pass, and the ranged
 * band info the combat brain positions with. Spell ids are the classic
 * WotLK rank-1 ids; the helper Rank() resolves the highest rank the character
 * actually knows, so a level-8 bot uses rank 3 and a level-80 bot uses the
 * last rank of the same ability.
 */

#ifndef PLAYERBOT_BOT_CLASS_AI_H
#define PLAYERBOT_BOT_CLASS_AI_H

#include "Define.h"
#include "SharedDefines.h"

#include <cstdint>
#include <initializer_list>
#include <vector>

class Player;
class Unit;
class BotAI;
class BotSpells;

class BotClassAI
{
public:
    explicit BotClassAI(BotAI* ai) : _ai(ai) { }
    virtual ~BotClassAI() = default;

    /// combat priority list; called every AI tick while a victim is engaged
    virtual void CombatTick(BotAI& ai) = 0;
    /// self/friend preservation, called before the combat priority list
    virtual void HealTick(BotAI& ai) { }
    /// called once when the bot is fully idle out of combat
    virtual void IdleTick(BotAI& ai) { }

    /// wand shoot (5019), auto shot (75), or 0 for none
    virtual uint32 GetAutoRepeatSpell() const { return 0; }
    /// shortest distance at which the ranged attack can fire (ranged classes)
    virtual float GetMinRange() const { return 0.0f; }
    /// true when the class fights in melee (warrior, rogue, ...)
    virtual bool IsMeleeClass() const { return true; }

    /// lowest-health group/self member below threshold within spell range, or nullptr
    Unit* FindHealTarget(float healthPct, float range) const;

protected:
    /// highest known rank from a C array of ids (lowest -> highest), or 0
    uint32 RankFrom(uint32 const* ids, std::size_t count) const;
    template <std::size_t N>
    uint32 Rank(uint32 const (&ids)[N]) const { return RankFrom(ids, N); }

    bool CastOnVictim(uint32 spellId);
    bool CastOnSelf(uint32 spellId);
    bool CastOnUnit(Unit* target, uint32 spellId);
    /// victim already has this (rank-correct) aura?
    bool VictimHasAura(uint32 spellId) const;
    bool SelfHasAura(uint32 spellId) const;

    BotAI* _ai;
};

/// factory: one script per character class (never null)
BotClassAI* CreateBotClassAI(uint8 playerClass, BotAI* ai);

// per-class creators (BotClass<Class>.cpp)
BotClassAI* CreateWarriorAI(BotAI* ai);
BotClassAI* CreatePaladinAI(BotAI* ai);
BotClassAI* CreateHunterAI(BotAI* ai);
BotClassAI* CreateRogueAI(BotAI* ai);
BotClassAI* CreatePriestAI(BotAI* ai);
BotClassAI* CreateDeathKnightAI(BotAI* ai);
BotClassAI* CreateShamanAI(BotAI* ai);
BotClassAI* CreateMageAI(BotAI* ai);
BotClassAI* CreateWarlockAI(BotAI* ai);
BotClassAI* CreateDruidAI(BotAI* ai);

#endif
