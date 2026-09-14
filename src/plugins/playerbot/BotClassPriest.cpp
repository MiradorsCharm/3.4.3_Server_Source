/*
 * Playerbot AI - priest.
 *
 * Smite/Mind Blast dps with SW:Pain upkeep, party healing with Renew as the
 * filler, Power Word: Fortitude + Divine Spirit buffs, Dispel Magic/Cure
 * Disease cures and Resurrection for the dead.
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
    constexpr uint32 SW_PAIN[] = { 589, 594, 970, 992, 2767, 10892, 10893, 10894, 25367, 25368, 48124, 48125 };
    constexpr uint32 MIND_BLAST[] = { 8092, 8102, 8103, 8104, 8105, 8106, 10945, 10946, 10947, 25372, 25373, 25375, 48126, 48127 };
    constexpr uint32 SMITE[] = { 585, 591, 598, 990, 991, 10934, 10933, 25363, 25364, 48123 };
    constexpr uint32 MIND_FLAY[] = { 15407, 17311, 17312, 17313, 17314, 18807, 25362, 48155, 48156 };
    constexpr uint32 VAMPIRIC_TOUCH[] = { 34914, 34915, 34916, 48159, 48160 };
    constexpr uint32 SHADOWFIEND[] = { 34433 };
    constexpr uint32 POWER_WORD_SHIELD[] = { 17, 592, 600, 3747, 6065, 6066, 10898, 10899, 10900, 10901, 25217, 25218, 48065, 48066 };
    constexpr uint32 RENEW[] = { 139, 6074, 6075, 6076, 6077, 6078, 10952, 10953, 25221, 25315, 48067, 48068 };
    constexpr uint32 FLASH_HEAL[] = { 2061, 9473, 9474, 9475, 10915, 10916, 10917, 25233, 25235, 48071, 48072 };
    constexpr uint32 GREATER_HEAL[] = { 2060, 10963, 10964, 10965, 25313, 25314, 48063, 48062 };
    constexpr uint32 HEAL[] = { 2054, 2055, 6063, 6064 };
    constexpr uint32 INNER_FIRE[] = { 588, 7128, 602, 1006, 10951, 48168 };
    constexpr uint32 POWER_WORD_FORTITUDE[] = { 1243, 1244, 1245, 2791, 10937, 10938, 25389, 48161, 48162 };
    constexpr uint32 DIVINE_SPIRIT[] = { 14752, 14818, 14819, 48073, 48074 };
    constexpr uint32 RESURRECTION[] = { 2006, 2010, 10880, 10881, 20770, 25435, 48171, 48172 };
    constexpr uint32 DISPEL_MAGIC_SPELL[] = { 527 };
    constexpr uint32 CURE_DISEASE[] = { 528 };
}

class BotClassPriestAI : public BotClassAI
{
public:
    explicit BotClassPriestAI(BotAI* ai) : BotClassAI(ai) { }

    bool IsMeleeClass() const override { return false; }
    float GetMinRange() const override { return 0.0f; }

    void RezTick(BotAI& ai) override
    {
        if (uint32 rez = Rank(RESURRECTION))
            if (Unit* dead = ai.FindDeadPartyMember(30.0f))
                CastOnUnit(dead, rez);
    }

    void CureTick(BotAI& ai) override
    {
        if (uint32 dispel = Rank(DISPEL_MAGIC_SPELL))
            if (Unit* hurt = ai.FindDispelTarget(SpellInfo::GetDispelMask(DISPEL_MAGIC), 30.0f))
                if (CastOnUnit(hurt, dispel))
                    return;

        if (uint32 cure = Rank(CURE_DISEASE))
            if (Unit* hurt = ai.FindDispelTarget(SpellInfo::GetDispelMask(DISPEL_DISEASE), 30.0f))
                CastOnUnit(hurt, cure);
    }

    void BuffTick(BotAI& ai) override
    {
        for (Unit* member : ai.GetPartyUnits(30.0f, false))
        {
            if (uint32 fort = Rank(POWER_WORD_FORTITUDE))
                if (!UnitHasAura(fort, member) && CastOnUnit(member, fort))
                    return;
            if (uint32 spirit = Rank(DIVINE_SPIRIT))
                if (!UnitHasAura(spirit, member) && CastOnUnit(member, spirit))
                    return;
        }
    }

    void HealTick(BotAI& ai) override
    {
        if (Unit* hurt = FindHealTarget(65.0f, 30.0f))
        {
            Player* bot = ai.GetBot();
            if (hurt == bot)
            {
                if (bot->GetHealthPct() < 35.0f && !SelfHasAura(Rank(POWER_WORD_SHIELD)) && CastOnSelf(Rank(POWER_WORD_SHIELD)))
                    return;
                if (bot->GetHealthPct() < 50.0f && !SelfHasAura(Rank(RENEW)) && CastOnSelf(Rank(RENEW)))
                    return;
                if (bot->GetHealthPct() < 60.0f && CastOnSelf(Rank(FLASH_HEAL)))
                    return;
                if (bot->GetHealthPct() < 80.0f && CastOnSelf(Rank(GREATER_HEAL)))
                    return;
            }
            else
            {
                if (hurt->GetHealthPct() < 50.0f && !UnitHasAura(Rank(RENEW), hurt) && CastOnUnit(hurt, Rank(RENEW)))
                    return;
                if (hurt->GetHealthPct() < 65.0f && CastOnUnit(hurt, Rank(FLASH_HEAL)))
                    return;
                if (hurt->GetHealthPct() < 85.0f && CastOnUnit(hurt, Rank(GREATER_HEAL)))
                    return;
            }
        }
    }

    void CombatTick(BotAI& ai) override
    {
        if (!ai.GetCombat().GetVictim())
            return;
        HealTick(ai);
        if (!SelfHasAura(Rank(POWER_WORD_FORTITUDE)))
            CastOnSelf(Rank(POWER_WORD_FORTITUDE));
        if (!SelfHasAura(Rank(INNER_FIRE)))
            CastOnSelf(Rank(INNER_FIRE));
        if (!VictimHasAura(Rank(SW_PAIN)))
            if (CastOnVictim(Rank(SW_PAIN)))
                return;
        if (!VictimHasAura(Rank(VAMPIRIC_TOUCH)))
            if (CastOnVictim(Rank(VAMPIRIC_TOUCH)))
                return;
        if (ai.GetBot()->GetPower(POWER_MANA) > ai.GetBot()->GetMaxPower(POWER_MANA) / 2)
            if (CastOnVictim(Rank(SHADOWFIEND)))
                return;
        if (CastOnVictim(Rank(MIND_BLAST)))
            return;
        if (CastOnVictim(Rank(MIND_FLAY)))
            return;
        if (CastOnVictim(Rank(SMITE)))
            return;
    }
};

BotClassAI* CreatePriestAI(BotAI* ai) { return new BotClassPriestAI(ai); }
