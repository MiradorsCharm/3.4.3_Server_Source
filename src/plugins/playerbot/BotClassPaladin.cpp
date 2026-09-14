/*
 * Playerbot AI - paladin.
 *
 * Seal + Judgement + Crusader Strike rotation, Consecration against melee
 * packs, Flash of Light/Holy Light for self and friends, Lay on Hands as the
 * last resort.
 */

#include "BotClassAI.h"
#include "BotAI.h"
#include "BotCombat.h"
#include "BotSpells.h"
#include "Player.h"
#include "Unit.h"

namespace
{
    constexpr uint32 SEAL_OF_RIGHTEOUSNESS[] = { 20154, 20287, 20288, 20289, 20290, 20291, 20292, 20293, 27155, 48781, 48782 };
    constexpr uint32 JUDGEMENT[] = { 20271, 19805, 19806, 19807, 19808, 27172, 53408 };
    constexpr uint32 CRUSADER_STRIKE[] = { 35395, 20473, 20918, 20919, 20920, 27175, 35393, 35394, 35395 };
    constexpr uint32 DIVINE_STORM[] = { 53385 };
    constexpr uint32 HAMMER_OF_JUSTICE[] = { 853, 5588, 5589, 10308 };
    constexpr uint32 CONSECRATION[] = { 26573, 20116, 20922, 20923, 20924, 20925, 27173, 48818, 48819 };
    constexpr uint32 EXORCISM[] = { 879, 5614, 5615, 10312, 10313, 10314, 27157, 48820, 48821 };
    constexpr uint32 FLASH_OF_LIGHT[] = { 19750, 19939, 19940, 19941, 19942, 19943, 27137, 48784, 48785 };
    constexpr uint32 HOLY_LIGHT[] = { 635, 639, 647, 1026, 1042, 3472, 10328, 10329, 25292, 27135, 27136, 48781, 48782 };
    constexpr uint32 LAY_ON_HANDS[] = { 633, 2800, 27154, 48788 };
    constexpr uint32 DIVINE_PLEA[] = { 54428 };
}

class BotClassPaladinAI : public BotClassAI
{
public:
    explicit BotClassPaladinAI(BotAI* ai) : BotClassAI(ai) { }

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
            CastOnUnit(friend_, Rank(FLASH_OF_LIGHT));
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
