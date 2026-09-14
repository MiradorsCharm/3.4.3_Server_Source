/*
 * Playerbot AI - hunter.
 *
 * Auto Shot is the bread and butter and is driven by the core's auto-repeat
 * loop once started; the script adds Serpent Sting, Arcane/Steady Shot and
 * aspect upkeep, and keeps the bot out of the (weapon-dependent) minimum
 * range by retreating.
 */

#include "BotClassAI.h"
#include "BotAI.h"
#include "BotCombat.h"
#include "BotSpells.h"
#include "Player.h"
#include "Pet.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <algorithm>
#include "Unit.h"

namespace
{
    constexpr uint32 AUTO_SHOT[] = { 75 };
    constexpr uint32 SERPENT_STING[] = { 1978, 13549, 13550, 13551, 13552, 13553, 13554, 13555, 25295, 27016, 49000, 49001 };
    constexpr uint32 ARCANE_SHOT[] = { 3044, 14281, 14282, 14283, 14284, 14285, 14286, 14287, 27019, 49044, 49045 };
    constexpr uint32 STEADY_SHOT[] = { 56641, 34120, 49051, 49052 };
    constexpr uint32 MULTI_SHOT[] = { 2643, 14288, 14289, 14290, 14291, 25294, 27021, 35578, 49047, 49048 };
    constexpr uint32 KILL_COMMAND[] = { 34026, 34472 };
    constexpr uint32 RAPTOR_STRIKE[] = { 2973, 14260, 14261, 14262, 14263, 14264, 14265, 14266, 27014, 48996, 48997 };
    constexpr uint32 ASPECT_OF_THE_HAWK[] = { 13165, 14318, 14319, 14320, 14321, 14322, 25296, 27044, 48998, 49002 };
    constexpr uint32 ASPECT_OF_THE_DRAGONHAWK[] = { 61846, 61847 };
    constexpr uint32 MEND_PET[] = { 136, 3111, 3661, 3662, 13542, 13543, 13544, 24640, 27046, 27047, 27048, 48989, 48990 };
}

class BotClassHunterAI : public BotClassAI
{
public:
    explicit BotClassHunterAI(BotAI* ai) : BotClassAI(ai) { }

    bool IsMeleeClass() const override { return false; }
    // The retreat threshold must match the data, not an assumption: the DBC
    // min range of the auto shot spell is authoritative (classic data had
    // both 8 and 5 yard values across patches). Self-adapting keeps the
    // 5-8yd band from ever becoming a dead zone again.
    float GetMinRange() const override
    {
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(Rank(AUTO_SHOT), DIFFICULTY_NONE))
            return std::max(info->GetMinRange(true), 1.0f);
        return 5.0f;
    }
    uint32 GetAutoRepeatSpell() const override { return Rank(AUTO_SHOT); }

    void HealTick(BotAI& ai) override
    {
        // Mend Pet keeps the pet alive; the hunter itself potions at v0.1 of
        // the AI - out of scope here.
        if (Pet* pet = ai.GetBot()->GetPet())
            if (pet->GetHealthPct() < 50.0f)
                CastOnUnit(pet, Rank(MEND_PET));
    }

    void CombatTick(BotAI& ai) override
    {
        Unit* victim = ai.GetCombat().GetVictim();
        if (!victim)
            return;

        HealTick(ai);

        // aspect upkeep
        if (!SelfHasAura(Rank(ASPECT_OF_THE_HAWK)) && !SelfHasAura(Rank(ASPECT_OF_THE_DRAGONHAWK)))
        {
            if (uint32 dragonhawk = Rank(ASPECT_OF_THE_DRAGONHAWK))
                CastOnSelf(dragonhawk);
            else if (uint32 hawk = Rank(ASPECT_OF_THE_HAWK))
                CastOnSelf(hawk);
        }

        // melee range fallback: raptor strike keeps us dangerous if a mob
        // closes inside the dead zone
        if (ai.GetBot()->GetExactDist(victim) < 8.0f)
            CastOnVictim(Rank(RAPTOR_STRIKE));

        if (!VictimHasAura(Rank(SERPENT_STING)))
            if (CastOnVictim(Rank(SERPENT_STING)))
                return;

        if (CastOnVictim(Rank(KILL_COMMAND)))
            return;
        if (CastOnVictim(Rank(STEADY_SHOT)))
            return;
        if (CastOnVictim(Rank(ARCANE_SHOT)))
            return;
        if (victim->IsPlayer())
            return;
        if (CastOnVictim(Rank(MULTI_SHOT)))
            return;

        // quiet rotation -> keep the auto shot loop running
        if (uint32 shot = GetAutoRepeatSpell())
            ai.GetSpells().StartAutoRepeat(shot, victim);
    }
};

BotClassAI* CreateHunterAI(BotAI* ai) { return new BotClassHunterAI(ai); }
