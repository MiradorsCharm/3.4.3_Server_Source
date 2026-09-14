/*
 * Playerbot AI - warrior.
 *
 * Battle Shout upkeep, a simple melee priority, a Charge gap-closer and the
 * tank role (Defensive Stance + Taunt + Demoralizing Shout) for the "tank"
 * command. Swings themselves are the core's job - the script only spends
 * GCDs and rage.
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
    constexpr uint32 CHARGE[] = { 100, 6178, 11578, 25275, 47975, 47976 };
    constexpr uint32 DEFENSIVE_STANCE[] = { 71 };
    constexpr uint32 TAUNT[] = { 355 };
    constexpr uint32 DEMO_SHOUT[] = { 1160, 6190, 11554, 11555, 11556, 25203, 47437 };
    constexpr uint32 SHIELD_SLAM[] = { 23922, 23923, 23924, 23925, 25258, 30356, 47487, 47488 };
}

class BotClassWarriorAI : public BotClassAI
{
public:
    explicit BotClassWarriorAI(BotAI* ai) : BotClassAI(ai) { }

    bool CanTank() const override { return true; }

    void TankTick(BotAI& ai) override
    {
        // Defensive Stance is the tank's resting state
        if (uint32 stance = Rank(DEFENSIVE_STANCE))
            if (!SelfHasAura(stance))
                CastOnSelf(stance);

        // pull whatever is chewing on the party back onto us
        if (uint32 taunt = Rank(TAUNT))
            for (Unit* member : ai.GetPartyUnits(35.0f, false))
            {
                Unit* thief = nullptr;
                for (Unit* attacker : member->getAttackers())
                    if (attacker && attacker->IsAlive() && attacker != ai.GetCombat().GetVictim())
                    {
                        thief = attacker;
                        break;
                    }
                if (thief && CastOnUnit(thief, taunt))
                {
                    ai.GetCombat().SetVictim(thief, "tank: taunted");
                    return;
                }
            }

        // demoralize whatever stands close to us
        if (Unit* victim = ai.GetCombat().GetVictim())
            if (ai.GetBot()->GetExactDist(victim) < 12.0f)
                CastOnVictim(Rank(DEMO_SHOUT));
    }

    void HealTick(BotAI& ai) override
    {
        // warriors have no self heal - Victory Rush after a kill is the only
        // recovery and it belongs to the rotation
        (void)ai;
    }

    void CombatTick(BotAI& ai) override
    {
        Unit* victim = ai.GetCombat().GetVictim();
        if (!victim)
            return;

        if (uint32 shout = Rank(BATTLE_SHOUT))
            if (!SelfHasAura(shout))
                CastOnSelf(shout);

        // close the gap while the mob is not swinging on us yet
        float const dist = ai.GetBot()->GetExactDist(victim);
        if (dist > 10.0f && dist < 25.0f && CastOnVictim(Rank(CHARGE)))
            return;

        if (victim->HealthBelowPct(20) && CastOnVictim(Rank(EXECUTE)))
            return;
        if (CastOnVictim(Rank(SHIELD_SLAM)))
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
