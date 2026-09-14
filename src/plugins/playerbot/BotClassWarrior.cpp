/*
 * Playerbot AI - warrior.
 *
 * Keep Battle Shout up, Mortal Strike/Bloodthirst on cooldown, Overpower and
 * Revenge reactively, Execute below 20%, Heroic Strike as rage dump.
 */

#include "BotClassAI.h"
#include "BotAI.h"
#include "BotCombat.h"
#include "BotSpells.h"
#include "Player.h"
#include "Unit.h"

namespace
{
    constexpr uint32 BATTLE_SHOUT[] = { 6673, 6192, 11574, 11575, 11576, 11577, 2048, 21551, 21552, 25289, 47436 };
    constexpr uint32 HEROIC_STRIKE[] = { 78, 284, 285, 1608, 11564, 11565, 11566, 11567, 25286, 29707, 30324 };
    constexpr uint32 MORTAL_STRIKE[] = { 12294, 21553, 21554, 21555, 21556, 25248, 30330, 33028 };
    constexpr uint32 BLOODTHIRST[] = { 23881, 23892, 23893, 23894, 25251, 30335, 33025 };
    constexpr uint32 OVERPOWER[] = { 7384, 7887, 11584, 11585, 11586, 25266 };
    constexpr uint32 REVENGE[] = { 6572, 7379, 11600, 11601, 25288, 25269, 47486 };
    constexpr uint32 EXECUTE[] = { 5308, 20658, 20660, 20661, 20662, 25234, 25236, 47470, 47471 };
    constexpr uint32 VICTORY_RUSH[] = { 34428 };
}

class BotClassWarriorAI : public BotClassAI
{
public:
    explicit BotClassWarriorAI(BotAI* ai) : BotClassAI(ai) { }

    void CombatTick(BotAI& ai) override
    {
        if (!ai.GetCombat().GetVictim())
            return;

        if (uint32 shout = Rank(BATTLE_SHOUT))
            if (!SelfHasAura(shout))
                CastOnSelf(shout);

        if (Unit* victim = ai.GetCombat().GetVictim())
            if (victim->HealthBelowPct(20) && CastOnVictim(Rank(EXECUTE)))
                return;

        if (CastOnVictim(Rank(MORTAL_STRIKE)))
            return;
        if (CastOnVictim(Rank(BLOODTHIRST)))
            return;
        if (CastOnVictim(Rank(REVENGE)))
            return;
        if (CastOnVictim(Rank(OVERPOWER)))
            return;
        if (CastOnVictim(Rank(VICTORY_RUSH)))
            return;

        // rage dump
        if (ai.GetBot()->GetPower(POWER_RAGE) >= 300)
            CastOnVictim(Rank(HEROIC_STRIKE));
    }
};

BotClassAI* CreateWarriorAI(BotAI* ai) { return new BotClassWarriorAI(ai); }
