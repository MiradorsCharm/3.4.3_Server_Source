/*
 * Playerbot AI - death knight.
 *
 * Diseases first (Icy Touch, Plague Strike), then Blood Strike/Obliterate,
 * Death Coil as the ranged filler, Horn of Winter upkeep, Death Grip to pull
 * runners back.
 */

#include "BotClassAI.h"
#include "BotAI.h"
#include "BotCombat.h"
#include "BotSpells.h"
#include "Player.h"
#include "Unit.h"

namespace
{
    constexpr uint32 ICY_TOUCH[] = { 45477, 49890, 49909, 55587, 55588 };
    constexpr uint32 PLAGUE_STRIKE[] = { 45462, 49889, 49917, 49918, 49919 };
    constexpr uint32 BLOOD_STRIKE[] = { 45902, 49927, 49928, 49929, 49930 };
    constexpr uint32 OBLITERATE[] = { 49020, 51423, 51424, 51425 };
    constexpr uint32 DEATH_STRIKE[] = { 49998, 49999, 50000, 50001 };
    constexpr uint32 HEART_STRIKE[] = { 55050, 55262, 55265, 55268, 55271 };
    constexpr uint32 FROST_STRIKE[] = { 49143, 51416, 51417, 51418, 51419, 55268 };
    constexpr uint32 DEATH_COIL[] = { 47541, 49892, 49893, 49894, 49895 };
    constexpr uint32 HORN_OF_WINTER[] = { 57330, 57623 };
    constexpr uint32 DEATH_GRIP[] = { 49576 };
    constexpr uint32 OUTBREAK_NONE = 0;
}

class BotClassDeathKnightAI : public BotClassAI
{
public:
    explicit BotClassDeathKnightAI(BotAI* ai) : BotClassAI(ai) { }

    void CombatTick(BotAI& ai) override
    {
        Player* bot = ai.GetBot();
        Unit* victim = ai.GetCombat().GetVictim();
        if (!victim)
            return;

        if (!SelfHasAura(Rank(HORN_OF_WINTER)))
            CastOnSelf(Rank(HORN_OF_WINTER));

        // pull ranged runners back into swing range
        if (!bot->IsWithinMeleeRange(victim) && victim->IsPlayer())
            if (CastOnVictim(Rank(DEATH_GRIP)))
                return;

        if (!VictimHasAura(Rank(ICY_TOUCH)))
            if (CastOnVictim(Rank(ICY_TOUCH)))
                return;
        if (!VictimHasAura(Rank(PLAGUE_STRIKE)))
            if (CastOnVictim(Rank(PLAGUE_STRIKE)))
                return;

        if (CastOnVictim(Rank(OBLITERATE)))
            return;
        if (CastOnVictim(Rank(HEART_STRIKE)))
            return;
        if (CastOnVictim(Rank(BLOOD_STRIKE)))
            return;
        if (bot->GetHealthPct() < 60.0f && CastOnVictim(Rank(DEATH_STRIKE)))
            return;
        if (CastOnVictim(Rank(FROST_STRIKE)))
            return;

        // no runes? Death Coil is free of them.
        if (CastOnVictim(Rank(DEATH_COIL)))
            return;
    }
};

BotClassAI* CreateDeathKnightAI(BotAI* ai) { return new BotClassDeathKnightAI(ai); }
