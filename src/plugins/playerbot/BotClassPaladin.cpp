/*
 * Playerbot AI - paladin.
 *
 * Seal/Judgement melee priority with party healing, Blessings, cures,
 * Redemption resurrection and the tank role (Righteous Fury + Hand of
 * Reckoning).
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
    constexpr uint32 SEAL_OF_RIGHTEOUSNESS[] = { 20154, 20287, 20288, 20289, 20290, 20291, 20292, 20293, 27155, 48956, 48957 };
    constexpr uint32 JUDGEMENT[] = { 20271, 19805, 19806, 19807, 19808, 27172, 53408 };
    constexpr uint32 CRUSADER_STRIKE[] = { 35395 };
    constexpr uint32 DIVINE_STORM[] = { 53385 };
    constexpr uint32 HAMMER_OF_JUSTICE[] = { 853, 5588, 5589, 10308 };
    constexpr uint32 CONSECRATION[] = { 26573, 20116, 20922, 20923, 20924, 20925, 27173, 48818, 48819 };
    constexpr uint32 EXORCISM[] = { 879, 5614, 5615, 10312, 10313, 10314, 27157, 48820, 48821 };
    constexpr uint32 FLASH_OF_LIGHT[] = { 19750, 19939, 19940, 19941, 19942, 19943, 27137, 48784, 48785 };
    constexpr uint32 HOLY_LIGHT[] = { 635, 639, 647, 894, 1026, 1042, 3472, 10328, 10329, 25292, 27135, 27136, 48781, 48782 };
    constexpr uint32 LAY_ON_HANDS[] = { 633, 2800, 27154, 48788 };
    constexpr uint32 DIVINE_PLEA[] = { 54428 };
    constexpr uint32 BLESSING_OF_KINGS[] = { 20217 };
    constexpr uint32 BLESSING_OF_MIGHT[] = { 19740, 19834, 19835, 19836, 19837, 19838, 25291, 27140, 48932 };
    constexpr uint32 BLESSING_OF_WISDOM[] = { 19742, 19850, 19852, 19853, 19854, 19855, 25894, 27143, 48938 };
    constexpr uint32 PURIFY[] = { 1152 };
    constexpr uint32 CLEANSE[] = { 4987 };
    constexpr uint32 REDEMPTION[] = { 7328, 10322, 20772, 20773, 20774 };
    constexpr uint32 RIGHTEOUS_FURY[] = { 25780 };
    constexpr uint32 HAND_OF_RECKONING[] = { 62124 };
}

class BotClassPaladinAI : public BotClassAI
{
public:
    explicit BotClassPaladinAI(BotAI* ai) : BotClassAI(ai) { }

    bool CanTank() const override { return true; }

    void TankTick(BotAI& ai) override
    {
        if (uint32 fury = Rank(RIGHTEOUS_FURY))
            if (!SelfHasAura(fury))
                CastOnSelf(fury);

        if (uint32 reckoning = Rank(HAND_OF_RECKONING))
            for (Unit* member : ai.GetPartyUnits(35.0f, false))
            {
                Unit* thief = nullptr;
                for (Unit* attacker : member->getAttackers())
                    if (attacker && attacker->IsAlive() && attacker != ai.GetCombat().GetVictim())
                    {
                        thief = attacker;
                        break;
                    }
                if (thief && CastOnUnit(thief, reckoning))
                {
                    ai.GetCombat().SetVictim(thief, "tank: taunted");
                    return;
                }
            }
    }

    void RezTick(BotAI& ai) override
    {
        if (uint32 rez = Rank(REDEMPTION))
            if (Unit* dead = ai.FindDeadPartyMember(30.0f))
                CastOnUnit(dead, rez);
    }

    void CureTick(BotAI& ai) override
    {
        // cleanse covers magic as well; purify is the cheap early option
        if (uint32 cleanse = Rank(CLEANSE))
            if (Unit* hurt = ai.FindDispelTarget(SpellInfo::GetDispelMask(DISPEL_MAGIC)
                | SpellInfo::GetDispelMask(DISPEL_POISON) | SpellInfo::GetDispelMask(DISPEL_DISEASE), 30.0f))
                if (CastOnUnit(hurt, cleanse))
                    return;

        if (uint32 purify = Rank(PURIFY))
            if (Unit* hurt = ai.FindDispelTarget(SpellInfo::GetDispelMask(DISPEL_POISON)
                | SpellInfo::GetDispelMask(DISPEL_DISEASE), 30.0f))
                CastOnUnit(hurt, purify);
    }

    void BuffTick(BotAI& ai) override
    {
        // Kings for everyone when known, otherwise might for melee and
        // wisdom for casters. One cast per pass; the next pass continues.
        for (Unit* member : ai.GetPartyUnits(30.0f, false))
        {
            if (uint32 kings = Rank(BLESSING_OF_KINGS))
            {
                if (!UnitHasAura(kings, member) && CastOnUnit(member, kings))
                    return;
                continue;
            }
            bool const caster = member->GetPowerType() == POWER_MANA;
            uint32 const buff = caster ? Rank(BLESSING_OF_WISDOM) : Rank(BLESSING_OF_MIGHT);
            if (buff && !UnitHasAura(buff, member) && CastOnUnit(member, buff))
                return;
        }
    }

    void HealTick(BotAI& ai) override
    {
        Player* bot = ai.GetBot();
        if (bot->GetHealthPct() < 25.0f)
            CastOnSelf(Rank(LAY_ON_HANDS));
        if (bot->GetHealthPct() < 60.0f)
            CastOnSelf(Rank(FLASH_OF_LIGHT));
        if (bot->GetPower(POWER_MANA) < bot->GetMaxPower(POWER_MANA) / 5)
            CastOnSelf(Rank(DIVINE_PLEA));
        if (Unit* friend_ = FindHealTarget(60.0f, 25.0f))
        {
            if (friend_->GetHealthPct() < 35.0f)
                CastOnUnit(friend_, Rank(HOLY_LIGHT));
            else
                CastOnUnit(friend_, Rank(FLASH_OF_LIGHT));
        }
    }

    void CombatTick(BotAI& ai) override
    {
        if (!ai.GetCombat().GetVictim())
            return;
        HealTick(ai);
        if (uint32 seal = Rank(SEAL_OF_RIGHTEOUSNESS))
            if (!SelfHasAura(seal))
                CastOnSelf(seal);
        if (CastOnVictim(Rank(JUDGEMENT)))
            return;
        if (CastOnVictim(Rank(CRUSADER_STRIKE)))
            return;
        if (CastOnVictim(Rank(DIVINE_STORM)))
            return;
        if (CastOnVictim(Rank(EXORCISM)))
            return;
        if (CastOnVictim(Rank(HAMMER_OF_JUSTICE)))
            return;
        if (Unit* victim = ai.GetCombat().GetVictim())
            if (!victim->IsPlayer() && CastOnVictim(Rank(CONSECRATION)))
                return;
    }
};

BotClassAI* CreatePaladinAI(BotAI* ai) { return new BotClassPaladinAI(ai); }
