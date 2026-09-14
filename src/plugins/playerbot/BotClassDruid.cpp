/*
 * Playerbot AI - druid.
 *
 * Casts from safety (Wrath/Starfire with Moonfire and Insect Swarm uptime),
 * heals itself and friends (Rejuvenation, Regrowth, Healing Touch), Mark of
 * the Wild and Thorns upkeep.
 */

#include "BotClassAI.h"
#include "BotAI.h"
#include "BotCombat.h"
#include "BotSpells.h"
#include "Player.h"
#include "Unit.h"

namespace
{
    constexpr uint32 WRATH[] = { 5176, 3376, 3377, 9912, 26978, 26979, 48460, 48461 };
    constexpr uint32 STARFIRE[] = { 2912, 8949, 8950, 8951, 9849, 25302, 26986, 48464, 48465 };
    constexpr uint32 MOONFIRE[] = { 8921, 8924, 8925, 8926, 8927, 8928, 8929, 9833, 9834, 9835, 26987, 26988, 48462, 48463 };
    constexpr uint32 INSECT_SWARM[] = { 5570, 24974, 24975, 24976, 27012, 27013, 48467, 48468 };
    constexpr uint32 ENTANGLING_ROOTS[] = { 339, 1062, 5195, 5196, 7812, 19971, 19972, 19973, 19974, 26989, 53308 };
    constexpr uint32 THORNS[] = { 467, 782, 1075, 8914, 9756, 9910, 26992, 53307 };
    constexpr uint32 MARK_OF_THE_WILD[] = { 1126, 5232, 6756, 8907, 9884, 9885, 26990, 26991, 48469, 48470 };
    constexpr uint32 REJUVENATION[] = { 774, 1058, 1430, 2090, 2091, 3627, 8910, 9839, 9840, 9841, 25299, 27051, 48440, 48441 };
    constexpr uint32 REGROWTH[] = { 8936, 8938, 8939, 8940, 8941, 9750, 9751, 9856, 9857, 9858, 25297, 26980, 48442, 48443 };
    constexpr uint32 HEALING_TOUCH[] = { 5185, 5186, 5187, 5188, 5189, 6778, 8903, 9758, 9888, 9889, 25297, 26981, 26982, 48443 };
    constexpr uint32 CAT_FORM[] = { 768 };
    constexpr uint32 SWIPE[] = { 779 };
}

class BotClassDruidAI : public BotClassAI
{
public:
    explicit BotClassDruidAI(BotAI* ai) : BotClassAI(ai) { }

    bool IsMeleeClass() const override { return false; }
    float GetMinRange() const override { return 0.0f; }

    void IdleTick(BotAI& ai) override
    {
        if (!SelfHasAura(Rank(MARK_OF_THE_WILD)))
            CastOnSelf(Rank(MARK_OF_THE_WILD));
        if (!SelfHasAura(Rank(THORNS)))
            CastOnSelf(Rank(THORNS));
    }

    void HealTick(BotAI& ai) override
    {
        if (Unit* hurt = FindHealTarget(65.0f, 30.0f))
        {
            if (hurt == ai.GetBot())
            {
                if (ai.GetBot()->GetHealthPct() < 70.0f && !SelfHasAura(Rank(REJUVENATION)) && CastOnSelf(Rank(REJUVENATION)))
                    return;
                if (ai.GetBot()->GetHealthPct() < 55.0f && CastOnSelf(Rank(REGROWTH)))
                    return;
                if (ai.GetBot()->GetHealthPct() < 35.0f && CastOnSelf(Rank(HEALING_TOUCH)))
                    return;
            }
            else
            {
                if (hurt->GetHealthPct() < 60.0f && CastOnUnit(hurt, Rank(REJUVENATION)))
                    return;
                if (hurt->GetHealthPct() < 50.0f && CastOnUnit(hurt, Rank(REGROWTH)))
                    return;
                if (hurt->GetHealthPct() < 35.0f && CastOnUnit(hurt, Rank(HEALING_TOUCH)))
                    return;
            }
        }
    }

    void CombatTick(BotAI& ai) override
    {
        Unit* victim = ai.GetCombat().GetVictim();
        if (!victim)
            return;

        HealTick(ai);
        IdleTick(ai);

        // root melee mobs chewing on us
        Player* bot = ai.GetBot();
        if (bot->GetHealthPct() < 55.0f && bot->GetExactDist(victim) < 8.0f)
            if (CastOnVictim(Rank(ENTANGLING_ROOTS)))
                return;

        if (!VictimHasAura(Rank(MOONFIRE)))
            if (CastOnVictim(Rank(MOONFIRE)))
                return;
        if (!VictimHasAura(Rank(INSECT_SWARM)))
            if (CastOnVictim(Rank(INSECT_SWARM)))
                return;

        if (CastOnVictim(Rank(WRATH)))
            return;
        if (CastOnVictim(Rank(STARFIRE)))
            return;
    }
};

BotClassAI* CreateDruidAI(BotAI* ai) { return new BotClassDruidAI(ai); }
