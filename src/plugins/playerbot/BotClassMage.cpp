/*
 * Playerbot AI - mage.
 *
 * Frostbolt/Fireball nuking with Frost Nova/Blink survival, Counterspell
 * interrupts, Polymorph for player targets, Evocation, Arcane Intellect
 * party buffs, Remove Curse cures - and the party pantry: conjuring food
 * and water for the eat/drink pass.
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
    constexpr uint32 FIREBALL[] = { 133, 143, 145, 3140, 8400, 8401, 8402, 10148, 10149, 10150, 10151, 25306, 27070, 38692, 42832, 42833 };
    constexpr uint32 FROSTBOLT[] = { 116, 205, 837, 7322, 8406, 8407, 8408, 10179, 10180, 10181, 25304, 27071, 27072, 38697, 42841, 42842 };
    constexpr uint32 FIRE_BLAST[] = { 2136, 2137, 2138, 8412, 8413, 10197, 10199, 27078, 27079, 42872, 42873 };
    constexpr uint32 SCORCH[] = { 2948, 8444, 8445, 8446, 10205, 10206, 10207, 27073, 27074, 42858, 42859 };
    constexpr uint32 PYROBLAST[] = { 11366, 11677, 30982, 42889 };
    constexpr uint32 ARCANE_MISSILES[] = { 5143, 5144, 5145, 8416, 8417, 10272, 18819, 25345, 27075, 38699, 42843, 42846 };
    constexpr uint32 FROST_NOVA[] = { 122, 865, 6131, 10230, 27088, 42917 };
    constexpr uint32 BLINK[] = { 1953 };
    constexpr uint32 EVOCATION[] = { 12051 };
    constexpr uint32 ARCANE_INTELLECT[] = { 1459, 1460, 1461, 10156, 10157, 27126, 27127, 43002 };
    constexpr uint32 ICE_ARMOR[] = { 7302, 7320, 10219, 10220, 27124, 43008 };
    constexpr uint32 FROST_ARMOR[] = { 168, 7301, 2943 };
    constexpr uint32 MAGE_ARMOR[] = { 43024, 43023, 43022, 43021, 43020, 27125 };
    constexpr uint32 POLYMORPH[] = { 118, 12824, 12825, 12826, 28271, 28272, 61305 };
    constexpr uint32 COUNTERSPELL[] = { 2139 };
    constexpr uint32 REMOVE_CURSE[] = { 475 };
    constexpr uint32 CONJURE_FOOD[] = { 587, 597, 977, 3299, 8766, 27089, 33709 };
    constexpr uint32 CONJURE_WATER[] = { 5504, 5505, 5506, 37421, 27091, 33717 };
}

class BotClassMageAI : public BotClassAI
{
public:
    explicit BotClassMageAI(BotAI* ai) : BotClassAI(ai) { }

    bool IsMeleeClass() const override { return false; }
    float GetMinRange() const override { return 0.0f; }

    void CureTick(BotAI& ai) override
    {
        if (uint32 cure = Rank(REMOVE_CURSE))
            if (Unit* hurt = ai.FindDispelTarget(SpellInfo::GetDispelMask(DISPEL_CURSE), 30.0f))
                CastOnUnit(hurt, cure);
    }

    void BuffTick(BotAI& ai) override
    {
        for (Unit* member : ai.GetPartyUnits(30.0f, false))
            if (uint32 ai1 = Rank(ARCANE_INTELLECT))
                if (!UnitHasAura(ai1, member) && CastOnUnit(member, ai1))
                    return;
    }

    void IdleTick(BotAI& ai) override
    {
        if (!SelfHasAura(Rank(ARCANE_INTELLECT)))
            CastOnSelf(Rank(ARCANE_INTELLECT));
        if (!SelfHasAura(Rank(ICE_ARMOR)) && !SelfHasAura(Rank(FROST_ARMOR)) && !SelfHasAura(Rank(MAGE_ARMOR)))
        {
            if (uint32 ma = Rank(MAGE_ARMOR))
                CastOnSelf(ma);
            else if (uint32 ia = Rank(ICE_ARMOR))
                CastOnSelf(ia);
            else if (uint32 fa = Rank(FROST_ARMOR))
                CastOnSelf(fa);
        }

        // the party pantry: conjure food/water when we run dry (the eat and
        // drink pass reads these out of the bags)
        Player* bot = ai.GetBot();
        if (bot->GetPower(POWER_MANA) > bot->GetMaxPower(POWER_MANA) / 2)
        {
            if (!ai.HasConsumable(false))
                CastOnSelf(Rank(CONJURE_FOOD));
            else if (!ai.HasConsumable(true))
                CastOnSelf(Rank(CONJURE_WATER));
        }
    }

    void HealTick(BotAI& ai) override
    {
        // mages do not heal; they blink away when something is chewing on them
        if (ai.GetBot()->GetHealthPct() < 40.0f)
            if (Unit* victim = ai.GetCombat().GetVictim())
                if (ai.GetBot()->GetExactDist(victim) < 6.0f)
                {
                    if (CastOnVictim(Rank(FROST_NOVA)))
                        if (ai.GetBot()->GetPower(POWER_MANA) > ai.GetBot()->GetMaxPower(POWER_MANA) / 10)
                            CastOnSelf(Rank(BLINK));
                }
        if (ai.GetBot()->GetPower(POWER_MANA) < ai.GetBot()->GetMaxPower(POWER_MANA) / 10)
            CastOnSelf(Rank(EVOCATION));
    }

    void CombatTick(BotAI& ai) override
    {
        Unit* victim = ai.GetCombat().GetVictim();
        if (!victim)
            return;

        HealTick(ai);

        // interrupt dangerous casts
        if (victim->IsNonMeleeSpellCast(false))
            if (CastOnVictim(Rank(COUNTERSPELL)))
                return;

        // sheep enemy players so the fight becomes 1v1 for a while
        if (victim->IsPlayer() && !VictimHasAura(Rank(POLYMORPH)))
            if (CastOnVictim(Rank(POLYMORPH)))
                return;

        // anything in our face gets frozen before we move away
        Player* bot = ai.GetBot();
        if (bot->GetHealthPct() < 50.0f && bot->GetExactDist(victim) < 7.0f)
            CastOnVictim(Rank(FROST_NOVA));

        // instant finisher while running
        if (bot->GetExactDist(victim) < 20.0f && CastOnVictim(Rank(FIRE_BLAST)))
            return;

        if (CastOnVictim(Rank(PYROBLAST)))
            return;
        if (!VictimHasAura(Rank(SCORCH)))
            if (CastOnVictim(Rank(SCORCH)))
                return;
        if (CastOnVictim(Rank(FIREBALL)))
            return;
        if (CastOnVictim(Rank(FROSTBOLT)))
            return;
    }
};

BotClassAI* CreateMageAI(BotAI* ai) { return new BotClassMageAI(ai); }
