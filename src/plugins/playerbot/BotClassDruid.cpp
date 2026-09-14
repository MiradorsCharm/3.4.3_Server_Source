/*
 * Playerbot AI - druid.
 *
 * Caster/pew-pew rotation with Moonfire/Insect Swarm upkeep, party healing
 * (Rejuvenation -> Regrowth -> Healing Touch), Mark of the Wild + Thorns
 * buffs, Remove Corruption cures, Revive/Rebirth resurrection and the tank
 * role in Bear Form (Growl + Maul/Swipe).
 */

#include "BotClassAI.h"
#include "BotAI.h"
#include "BotCombat.h"
#include "BotSpells.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
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
    constexpr uint32 HEALING_TOUCH[] = { 5185, 5186, 5187, 5188, 5189, 6778, 8903, 9758, 9888, 9889, 26981, 26982 };
    constexpr uint32 CAT_FORM[] = { 768 };
    constexpr uint32 BEAR_FORM[] = { 5487 };
    constexpr uint32 GROWL[] = { 6795 };
    constexpr uint32 MAUL[] = { 6807, 6808, 6809, 8972, 9745, 9880, 9881, 26996 };
    constexpr uint32 SWIPE[] = { 779 };
    constexpr uint32 DEMORALIZING_ROAR[] = { 99, 17331, 26998, 48556 };
    constexpr uint32 REVIVE[] = { 50769, 50770, 50771, 50772 };
    constexpr uint32 REBIRTH[] = { 20484, 20739, 20742, 20747, 20748, 26994, 48477 };
    constexpr uint32 REMOVE_CORRUPTION[] = { 2782 };
}

class BotClassDruidAI : public BotClassAI
{
public:
    explicit BotClassDruidAI(BotAI* ai) : BotClassAI(ai) { }

    bool IsMeleeClass() const override { return false; }
    float GetMinRange() const override { return 0.0f; }
    bool CanTank() const override { return true; }

    void TankTick(BotAI& ai) override
    {
        // bear is the tank's resting form
        if (uint32 bear = Rank(BEAR_FORM))
            if (!SelfHasAura(bear) && CastOnSelf(bear))
                return;

        // pull whatever is chewing on the party back onto us
        if (uint32 growl = Rank(GROWL))
            for (Unit* member : ai.GetPartyUnits(30.0f, false))
            {
                Unit* thief = nullptr;
                for (Unit* attacker : member->getAttackers())
                    if (attacker && attacker->IsAlive() && attacker != ai.GetCombat().GetVictim())
                    {
                        thief = attacker;
                        break;
                    }
                if (thief && CastOnUnit(thief, growl))
                {
                    ai.GetCombat().SetVictim(thief, "tank: growled");
                    return;
                }
            }

        // bear fillers
        if (SelfHasAura(Rank(BEAR_FORM)))
        {
            if (CastOnVictim(Rank(MAUL)))
                return;
            if (CastOnVictim(Rank(DEMORALIZING_ROAR)))
                return;
        }
    }

    void RezTick(BotAI& ai) override
    {
        if (uint32 revive = Rank(REVIVE))
            if (Unit* dead = ai.FindDeadPartyMember(30.0f))
                CastOnUnit(dead, revive);
    }

    void CureTick(BotAI& ai) override
    {
        if (uint32 cure = Rank(REMOVE_CORRUPTION))
            if (Unit* hurt = ai.FindDispelTarget(SpellInfo::GetDispelMask(DISPEL_POISON)
                | SpellInfo::GetDispelMask(DISPEL_CURSE), 30.0f))
                CastOnUnit(hurt, cure);
    }

    void BuffTick(BotAI& ai) override
    {
        for (Unit* member : ai.GetPartyUnits(30.0f, false))
        {
            if (uint32 motw = Rank(MARK_OF_THE_WILD))
                if (!UnitHasAura(motw, member) && CastOnUnit(member, motw))
                    return;
            if (uint32 thorns = Rank(THORNS))
                if (!UnitHasAura(thorns, member) && CastOnUnit(member, thorns))
                    return;
        }
    }

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
                if (hurt->GetHealthPct() < 60.0f && !UnitHasAura(Rank(REJUVENATION), hurt) && CastOnUnit(hurt, Rank(REJUVENATION)))
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

        Player* bot = ai.GetBot();

        // in bear form the TankTick fillers already spend our GCDs; only the
        // emergency heal is left here
        if (SelfHasAura(Rank(BEAR_FORM)))
        {
            if (bot->GetHealthPct() < 35.0f && CastOnSelf(Rank(HEALING_TOUCH)))
                return;
            return;
        }

        IdleTick(ai);

        // root melee mobs chewing on us
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
