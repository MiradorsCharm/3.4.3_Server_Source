/*
 * Playerbot AI - death knight.
 *
 * Disease upkeep (Icy Touch + Plague Strike) into Obliterate/Blood Strike,
 * Death Coil as the rune-free filler, Death Grip for runners, Raise Dead
 * pet, Horn of Winter upkeep and the tank role in Frost Presence with Dark
 * Command.
 */

#include "BotClassAI.h"
#include "BotAI.h"
#include "BotCombat.h"
#include "BotSpells.h"
#include "Player.h"
#include "Unit.h"

namespace
{
    constexpr uint32 ICY_TOUCH[] = { 45477, 49890, 49909, 55587, 55588 };
    constexpr uint32 PLAGUE_STRIKE[] = { 45462, 49889, 49917, 49918, 49919 };
    constexpr uint32 BLOOD_STRIKE[] = { 45902, 49927, 49928, 49929, 49930 };
    constexpr uint32 OBLITERATE[] = { 49020, 51423, 51424, 51425 };
    constexpr uint32 DEATH_STRIKE[] = { 49998, 49999, 50000, 50001 };
    constexpr uint32 HEART_STRIKE[] = { 55050, 55262, 55265, 55271 };
    constexpr uint32 FROST_STRIKE[] = { 49143, 51416, 51417, 51418, 51419 };
    constexpr uint32 DEATH_COIL[] = { 47541, 49893, 49894, 49895, 49896 };
    constexpr uint32 HORN_OF_WINTER[] = { 57330, 57623 };
    constexpr uint32 DEATH_GRIP[] = { 49576 };
    constexpr uint32 RAISE_DEAD[] = { 46584 };
    constexpr uint32 FROST_PRESENCE[] = { 48266 };
    constexpr uint32 BLOOD_PRESENCE[] = { 48263 };
    constexpr uint32 DARK_COMMAND[] = { 56222 };
}

class BotClassDeathKnightAI : public BotClassAI
{
public:
    explicit BotClassDeathKnightAI(BotAI* ai) : BotClassAI(ai) { }

    bool CanTank() const override { return true; }

    void TankTick(BotAI& ai) override
    {
        // frost presence holds threat better; presence swap is free
        if (uint32 presence = Rank(FROST_PRESENCE))
            if (!SelfHasAura(presence))
                CastOnSelf(presence);

        if (uint32 command = Rank(DARK_COMMAND))
            for (Unit* member : ai.GetPartyUnits(30.0f, false))
            {
                Unit* thief = nullptr;
                for (Unit* attacker : member->getAttackers())
                    if (attacker && attacker->IsAlive() && attacker != ai.GetCombat().GetVictim())
                    {
                        thief = attacker;
                        break;
                    }
                if (thief && CastOnUnit(thief, command))
                {
                    ai.GetCombat().SetVictim(thief, "tank: dark command");
                    return;
                }
            }
    }

    void CombatTick(BotAI& ai) override
    {
        Player* bot = ai.GetBot();
        Unit* victim = ai.GetCombat().GetVictim();
        if (!victim)
            return;

        if (uint32 horn = Rank(HORN_OF_WINTER))
            if (!SelfHasAura(horn))
                CastOnSelf(horn);

        // keep a ghoul up once we can raise one
        if (!bot->GetPet() && bot->GetPower(POWER_RUNIC_POWER) < 20)
            CastOnSelf(Rank(RAISE_DEAD));

        // pull ranged runners back into swing range
        if (!bot->IsWithinMeleeRange(victim) && CastOnVictim(Rank(DEATH_GRIP)))
            return;

        if (!VictimHasAura(Rank(ICY_TOUCH)))
            if (CastOnVictim(Rank(ICY_TOUCH)))
                return;
        if (!VictimHasAura(Rank(PLAGUE_STRIKE)))
            if (CastOnVictim(Rank(PLAGUE_STRIKE)))
                return;
        if (CastOnVictim(Rank(OBLITERATE)))
            return;
        if (CastOnVictim(Rank(HEART_STRIKE)))
            return;
        if (CastOnVictim(Rank(BLOOD_STRIKE)))
            return;
        if (bot->GetHealthPct() < 60.0f && CastOnVictim(Rank(DEATH_STRIKE)))
            return;
        if (CastOnVictim(Rank(FROST_STRIKE)))
            return;

        // no runes? Death Coil is free of them.
        if (CastOnVictim(Rank(DEATH_COIL)))
            return;
    }
};

BotClassAI* CreateDeathKnightAI(BotAI* ai) { return new BotClassDeathKnightAI(ai); }
