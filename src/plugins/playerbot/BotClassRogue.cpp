/*
 * Playerbot AI - rogue.
 *
 * Build combo points with Sinister Strike (or Mutilate with daggers), finish
 * with Eviscerate, keep Slice and Dice up when fights last, Kick interrupts.
 */

#include "BotClassAI.h"
#include "BotAI.h"
#include "BotCombat.h"
#include "BotSpells.h"
#include "Player.h"
#include "Unit.h"

namespace
{
    constexpr uint32 SINISTER_STRIKE[] = { 1752, 1757, 1758, 1759, 1760, 8621, 8686, 11293, 11294, 26861, 26862, 48637, 48638 };
    constexpr uint32 MUTILATE[] = { 1329, 34411, 34412, 34413, 48665, 48666 };
    constexpr uint32 BACKSTAB[] = { 53, 2589, 2590, 2591, 7796, 8721, 11279, 11280, 11281, 25301, 26863, 48656, 48657 };
    constexpr uint32 EVISCERATE[] = { 2098, 6760, 6761, 6762, 8623, 8624, 11299, 11300, 31015, 26865, 48668, 48669 };
    constexpr uint32 SLICE_AND_DICE[] = { 5171, 6774, 1725 };
    constexpr uint32 RUPTURE[] = { 1943, 8639, 8640, 11273, 11274, 11275, 26867, 48671, 48672 };
    constexpr uint32 KICK[] = { 1766, 1767, 1768, 1769, 38768 };
    constexpr uint32 STEALTH[] = { 1784, 1785, 1786, 1787 };
    constexpr uint32 DEADLY_POISON[] = { 2823, 2818, 2819, 11353, 11354, 11355, 25347, 26867, 57993, 57994 };
    constexpr uint32 SAP[] = { 6770, 2070, 11297, 6770 };
}

class BotClassRogueAI : public BotClassAI
{
public:
    explicit BotClassRogueAI(BotAI* ai) : BotClassAI(ai) { }

    void CombatTick(BotAI& ai) override
    {
        Player* bot = ai.GetBot();
        Unit* victim = ai.GetCombat().GetVictim();
        if (!victim)
            return;

        // interrupt the victim when it starts a hard cast
        if (victim->IsNonMeleeSpellCast(false))
            if (CastOnVictim(Rank(KICK)))
                return;

        // upkeep: slice and dice once we have points and it fell off
        if (!SelfHasAura(Rank(SLICE_AND_DICE)))
            if (bot->GetPower(POWER_ENERGY) >= 25)
                if (CastOnSelf(Rank(SLICE_AND_DICE)))
                    return;

        // finishers
        if (victim->HealthBelowPct(35))
            if (CastOnVictim(Rank(EVISCERATE)))
                return;
        if (bot->GetComboPoints() >= 5)
            if (CastOnVictim(Rank(RUPTURE)) || CastOnVictim(Rank(EVISCERATE)))
                return;

        // builders
        if (CastOnVictim(Rank(MUTILATE)))
            return;
        if (CastOnVictim(Rank(SINISTER_STRIKE)))
            return;
        if (CastOnVictim(Rank(BACKSTAB)))
            return;
    }
};

BotClassAI* CreateRogueAI(BotAI* ai) { return new BotClassRogueAI(ai); }
