/*
 * Playerbot AI - shaman.
 *
 * Shock/lightning dps with Flame Shock upkeep, weapon imbues, shields,
 * party healing (LHW + Healing Wave + Chain Heal), Earth Shield on the
 * tank, Ancestral Spirit resurrection and Cure Toxins/Cleanse Spirit cures.
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
    constexpr uint32 EARTH_SHOCK[] = { 8042, 8044, 8045, 8046, 10412, 10413, 10414, 25454, 49230, 49231 };
    constexpr uint32 FLAME_SHOCK[] = { 8050, 8052, 8053, 10447, 10448, 29228, 30572, 49232, 49233 };
    constexpr uint32 LIGHTNING_BOLT[] = { 403, 529, 548, 915, 943, 6041, 10391, 10392, 15207, 15208, 25449, 49237, 49238 };
    constexpr uint32 CHAIN_LIGHTNING[] = { 421, 930, 2860, 10605, 25439, 25442, 49270, 49271 };
    constexpr uint32 LAVA_BURST[] = { 51505, 60043 };
    constexpr uint32 STORMSTRIKE[] = { 17364, 55610 };
    constexpr uint32 LAVA_LASH[] = { 60103 };
    constexpr uint32 WINDFURY_WEAPON[] = { 33757, 33731, 33735, 33736, 58804, 58796, 58789 };
    constexpr uint32 FLAMETONGUE_WEAPON[] = { 58788, 58787, 58785, 58784, 58783, 58782, 58781, 16361, 16359, 16358, 16357, 16356 };
    constexpr uint32 LIGHTNING_SHIELD[] = { 324, 325, 905, 945, 8134, 10431, 10432, 25469, 25472, 49279, 49280, 49281 };
    constexpr uint32 WATER_SHIELD[] = { 57960, 49284 };
    constexpr uint32 LESSER_HEALING_WAVE[] = { 8004, 8008, 8010, 10466, 10467, 10468, 49275, 49276 };
    constexpr uint32 HEALING_WAVE[] = { 331, 332, 547, 913, 939, 959, 8005, 10395, 10396, 25357, 25391, 49272, 49273 };
    constexpr uint32 CHAIN_HEAL[] = { 1064, 10622, 10623, 25422, 25423, 55458, 55459 };
    constexpr uint32 EARTH_SHIELD[] = { 974, 32593, 32594, 49283, 49284 };
    constexpr uint32 ANCESTRAL_SPIRIT[] = { 2008, 20609, 20610, 20611, 20612, 20613, 20776, 20777 };
    constexpr uint32 CURE_TOXINS[] = { 526 };
    constexpr uint32 CLEANSE_SPIRIT[] = { 51886 };
}

class BotClassShamanAI : public BotClassAI
{
public:
    explicit BotClassShamanAI(BotAI* ai) : BotClassAI(ai) { }

    bool IsMeleeClass() const override { return false; }
    float GetMinRange() const override { return 0.0f; }

    void RezTick(BotAI& ai) override
    {
        if (uint32 rez = Rank(ANCESTRAL_SPIRIT))
            if (Unit* dead = ai.FindDeadPartyMember(30.0f))
                CastOnUnit(dead, rez);
    }

    void CureTick(BotAI& ai) override
    {
        // cleanse spirit also removes curses (wrath behaviour)
        if (uint32 cleanse = Rank(CLEANSE_SPIRIT))
            if (Unit* hurt = ai.FindDispelTarget(SpellInfo::GetDispelMask(DISPEL_CURSE)
                | SpellInfo::GetDispelMask(DISPEL_POISON) | SpellInfo::GetDispelMask(DISPEL_DISEASE), 30.0f))
                if (CastOnUnit(hurt, cleanse))
                    return;

        if (uint32 cure = Rank(CURE_TOXINS))
            if (Unit* hurt = ai.FindDispelTarget(SpellInfo::GetDispelMask(DISPEL_POISON)
                | SpellInfo::GetDispelMask(DISPEL_DISEASE), 30.0f))
                CastOnUnit(hurt, cure);
    }

    void BuffTick(BotAI& ai) override
    {
        // earth shield on the first injured/leading member is the shaman's
        // party utility; skip when nobody needs it
        if (uint32 shield = Rank(EARTH_SHIELD))
            if (Unit* tank = ai.GetMaster())
                if (!UnitHasAura(shield, tank) && CastOnUnit(tank, shield))
                    return;
    }

    void HealTick(BotAI& ai) override
    {
        // a badly hurt friend outranks our own safety: healers die last
        if (Unit* friend_ = FindHealTarget(45.0f, 30.0f))
        {
            if (friend_->GetHealthPct() < 30.0f && CastOnUnit(friend_, Rank(HEALING_WAVE)))
                return;
            if (CastOnUnit(friend_, Rank(LESSER_HEALING_WAVE)))
                return;
        }

        if (ai.GetBot()->GetHealthPct() < 65.0f)
            if (CastOnSelf(Rank(LESSER_HEALING_WAVE)))
                return;
        if (ai.GetBot()->GetHealthPct() < 40.0f)
            if (CastOnSelf(Rank(HEALING_WAVE)))
                return;
    }

    void CombatTick(BotAI& ai) override
    {
        if (!ai.GetCombat().GetVictim())
            return;
        HealTick(ai);

        // weapon imbues (cast on self; they buff the equipped weapon)
        if (!SelfHasAura(Rank(WINDFURY_WEAPON)) && !SelfHasAura(Rank(FLAMETONGUE_WEAPON)))
        {
            if (uint32 wf = Rank(WINDFURY_WEAPON))
                CastOnSelf(wf);
            else if (uint32 ft = Rank(FLAMETONGUE_WEAPON))
                CastOnSelf(ft);
        }
        if (!SelfHasAura(Rank(LIGHTNING_SHIELD)) && !SelfHasAura(Rank(WATER_SHIELD)))
        {
            if (uint32 ls = Rank(LIGHTNING_SHIELD))
                CastOnSelf(ls);
            else if (uint32 ws = Rank(WATER_SHIELD))
                CastOnSelf(ws);
        }

        Unit* victim = ai.GetCombat().GetVictim();
        if (ai.GetBot()->GetExactDist(victim) < 10.0f)
        {
            if (!VictimHasAura(Rank(FLAME_SHOCK)))
                if (CastOnVictim(Rank(FLAME_SHOCK)))
                    return;
            if (CastOnVictim(Rank(STORMSTRIKE)))
                return;
            if (CastOnVictim(Rank(LAVA_LASH)))
                return;
            if (CastOnVictim(Rank(EARTH_SHOCK)))
                return;
        }
        else
        {
            if (!VictimHasAura(Rank(FLAME_SHOCK)))
                if (CastOnVictim(Rank(FLAME_SHOCK)))
                    return;
            // lava burst only erupts a burning flame shock
            if (VictimHasAura(Rank(FLAME_SHOCK)))
                if (CastOnVictim(Rank(LAVA_BURST)))
                    return;
            if (CastOnVictim(Rank(CHAIN_LIGHTNING)))
                return;
            if (CastOnVictim(Rank(LIGHTNING_BOLT)))
                return;
        }
    }
};

BotClassAI* CreateShamanAI(BotAI* ai) { return new BotClassShamanAI(ai); }
