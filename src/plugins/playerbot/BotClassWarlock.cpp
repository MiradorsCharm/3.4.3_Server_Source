/*
 * Playerbot AI - warlock.
 *
 * Demon Skin/Soul Link style upkeep, pet summon on login, Immolate ->
 * Corruption -> Curse uptime, Incinerate/Shadow Bolt filler, Drain Life when
 * hurt, Life Tap when dry.
 */

#include "BotClassAI.h"
#include "BotAI.h"
#include "BotCombat.h"
#include "BotSpells.h"
#include "Player.h"
#include "Unit.h"

namespace
{
    constexpr uint32 IMMOLATE[] = { 348, 707, 1094, 2941, 11665, 11667, 11668, 25309, 27215, 47810, 47811 };
    constexpr uint32 CORRUPTION[] = { 172, 6222, 6223, 7648, 11671, 11672, 25311, 27216, 47812, 47813 };
    constexpr uint32 CURSE_OF_AGONY[] = { 980, 1014, 6217, 11711, 11712, 11713, 27218, 27219, 47863, 47864 };
    constexpr uint32 SHADOW_BOLT[] = { 686, 696, 705, 1088, 1106, 7641, 11659, 11660, 11661, 25307, 27209, 47808, 47809 };
    constexpr uint32 INCINERATE[] = { 29722, 32231, 32232, 47837, 47838 };
    constexpr uint32 CONFLAGRATE[] = { 17962, 18930, 18931, 18932, 27249, 30911, 47824, 47825 };
    constexpr uint32 DRAIN_LIFE[] = { 689, 699, 709, 7651, 11693, 11694, 11695, 27219, 27220, 47841, 47842, 47843 };
    constexpr uint32 CHAOS_BOLT[] = { 50796, 59170, 59171, 59172 };
    constexpr uint32 LIFE_TAP[] = { 1454, 1455, 1456, 11687, 11688, 11689, 27222, 57946 };
    constexpr uint32 DEMON_SKIN[] = { 687, 697, 706, 1086, 11733, 11734, 11735, 28176, 47878 };
    constexpr uint32 FEL_ARMOR[] = { 47893, 47892, 47891, 28189, 47856 };
    constexpr uint32 SUMMON_IMP[] = { 688 };
    constexpr uint32 SUMMON_VOIDWALKER[] = { 697 };
    constexpr uint32 SUMMON_FELHUNTER[] = { 691 };
    constexpr uint32 SOUL_FIRE[] = { 6353, 17951, 17952, 11661 };
}

class BotClassWarlockAI : public BotClassAI
{
public:
    explicit BotClassWarlockAI(BotAI* ai) : BotClassAI(ai) { }

    bool IsMeleeClass() const override { return false; }
    float GetMinRange() const override { return 0.0f; }

    void IdleTick(BotAI& ai) override
    {
        Player* bot = ai.GetBot();
        if (!SelfHasAura(Rank(DEMON_SKIN)) && !SelfHasAura(Rank(FEL_ARMOR)))
        {
            if (uint32 fa = Rank(FEL_ARMOR))
                CastOnSelf(fa);
            else if (uint32 ds = Rank(DEMON_SKIN))
                CastOnSelf(ds);
        }

        if (!bot->GetPet())
        {
            // keep a demon out (voidwalker tanks better; imp is the fallback)
            if (bot->GetPower(POWER_MANA) > bot->GetMaxPower(POWER_MANA) / 3)
            {
                if (!CastOnSelf(Rank(SUMMON_VOIDWALKER)))
                    CastOnSelf(Rank(SUMMON_IMP));
            }
        }
    }

    void HealTick(BotAI& ai) override
    {
        if (ai.GetBot()->GetPower(POWER_MANA) < ai.GetBot()->GetMaxPower(POWER_MANA) / 6)
            CastOnSelf(Rank(LIFE_TAP));
        if (ai.GetBot()->GetHealthPct() < 65.0f)
            CastOnVictim(Rank(DRAIN_LIFE));
    }

    void CombatTick(BotAI& ai) override
    {
        Unit* victim = ai.GetCombat().GetVictim();
        if (!victim)
            return;

        HealTick(ai);

        if (!VictimHasAura(Rank(IMMOLATE)))
            if (CastOnVictim(Rank(IMMOLATE)))
                return;
        if (!VictimHasAura(Rank(CORRUPTION)))
            if (CastOnVictim(Rank(CORRUPTION)))
                return;
        if (!VictimHasAura(Rank(CURSE_OF_AGONY)))
            if (CastOnVictim(Rank(CURSE_OF_AGONY)))
                return;

        // conflagrate only pays off while immolate burns
        if (VictimHasAura(Rank(IMMOLATE)))
            if (CastOnVictim(Rank(CONFLAGRATE)))
                return;

        if (CastOnVictim(Rank(CHAOS_BOLT)))
            return;
        if (CastOnVictim(Rank(INCINERATE)))
            return;
        if (CastOnVictim(Rank(SHADOW_BOLT)))
            return;
        if (CastOnVictim(Rank(SOUL_FIRE)))
            return;

        // nothing off cooldown -> drain life keeps us alive while we wait
        if (ai.GetBot()->GetHealthPct() < 80.0f)
            CastOnVictim(Rank(DRAIN_LIFE));
    }
};

BotClassAI* CreateWarlockAI(BotAI* ai) { return new BotClassWarlockAI(ai); }
