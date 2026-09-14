#pragma once
#include "../Trigger.h"
#include "../../PlayerbotAIConfig.h"

namespace ai
{
    class EnemyTooCloseForSpellTrigger : public Trigger {
    public:
        EnemyTooCloseForSpellTrigger(PlayerbotAI* ai) : Trigger(ai, "enemy too close for spell") {}
        virtual bool IsActive()
		{
            Unit* target = AI_VALUE(Unit*, "current target");
            return target && AI_VALUE2(float, "distance", "current target") <= sPlayerbotAIConfig.spellDistance / 2;
        }
    };

    // A ranged bot inside melee reach of its victim is in the classic dead zone:
    // Shoot/wand is refused inside its minimum range, casts are interrupted, and
    // the wedged auto-repeat spell pauses the combat timers so even melee swings
    // stop. This is what made ranged bots stand nose-to-nose with a target doing
    // nothing ("cannot shoot and cannot swing") - the AI had no behaviour at all
    // for "too close", only for "too far".
    class EnemyInsideRangedDeadZoneTrigger : public Trigger {
    public:
        EnemyInsideRangedDeadZoneTrigger(PlayerbotAI* ai) : Trigger(ai, "enemy inside ranged dead zone") {}
        virtual bool IsActive()
        {
            if (!ai->IsRanged(bot))
                return false;

            Unit* target = AI_VALUE(Unit*, "current target");
            if (!target)
                return false;

            // Centre-to-centre, like every core range test. 11 yd is past the
            // 8-yd minimum range of Shoot/wand with margin; the matching action
            // walks back out to spellDistance/2, so trigger and action cannot
            // fight each other over the last yard.
            return bot->GetExactDist(target) < 11.0f;
        }
    };


    class EnemyTooCloseForMeleeTrigger : public Trigger {
    public:
        EnemyTooCloseForMeleeTrigger(PlayerbotAI* ai) : Trigger(ai, "enemy too close for melee", 5) {}
        virtual bool IsActive()
		{
			Unit* target = AI_VALUE(Unit*, "current target");
            return target && AI_VALUE2(float, "distance", "current target") <= sPlayerbotAIConfig.contactDistance / 2;
        }
    };

    class OutOfRangeTrigger : public Trigger {
    public:
        OutOfRangeTrigger(PlayerbotAI* ai, string name, float distance) : Trigger(ai, name)
		{
            this->distance = distance;
        }
        virtual bool IsActive()
		{
			Unit* target = AI_VALUE(Unit*, GetTargetName());
			return target && AI_VALUE2(float, "distance", GetTargetName()) > distance;
        }
        virtual string GetTargetName() { return "current target"; }

    protected:
        float distance;
    };

    class EnemyOutOfMeleeTrigger : public OutOfRangeTrigger
	{
    public:
        EnemyOutOfMeleeTrigger(PlayerbotAI* ai) : OutOfRangeTrigger(ai, "enemy out of melee range", sPlayerbotAIConfig.meleeDistance) {}
    };

    class EnemyOutOfSpellRangeTrigger : public OutOfRangeTrigger
	{
    public:
        EnemyOutOfSpellRangeTrigger(PlayerbotAI* ai) : OutOfRangeTrigger(ai, "enemy out of spell range", EffectiveSpellDistance(ai)) {}
    private:
        static float EffectiveSpellDistance(PlayerbotAI* ai)
        {
            Player* bot = ai->GetBot();
            if (!bot)
                return sPlayerbotAIConfig.spellDistance;
            // Mirror ReachSpellAction: wand/bow/gun users and pure casters can
            // cast out to ~30 yd; if the trigger uses the old 25 yd threshold
            // they never walk in for a 28-30 yd Shadow Bolt and the core keeps
            // rejecting the cast with SPELL_FAILED_OUT_OF_RANGE.
            if (bot->GetWeaponForAttack(RANGED_ATTACK, true) != nullptr
                || bot->GetClass() == CLASS_WARLOCK || bot->GetClass() == CLASS_MAGE
                || bot->GetClass() == CLASS_PRIEST || bot->GetClass() == CLASS_HUNTER)
                return 28.0f;
            return sPlayerbotAIConfig.spellDistance;
        }
    };

    class PartyMemberToHealOutOfSpellRangeTrigger : public OutOfRangeTrigger
	{
    public:
        PartyMemberToHealOutOfSpellRangeTrigger(PlayerbotAI* ai) : OutOfRangeTrigger(ai, "party member to heal out of spell range", sPlayerbotAIConfig.spellDistance) {}
        virtual string GetTargetName() { return "party member to heal"; }
    };

    class FarFromMasterTrigger : public Trigger {
    public:
        FarFromMasterTrigger(PlayerbotAI* ai, string name = "far from master", float distance = 12.0f, int checkInterval = 1) : Trigger(ai, name, checkInterval), distance(distance) {}

        virtual bool IsActive()
        {
            return AI_VALUE2(float, "distance", "master target") > distance;
        }

    private:
        float distance;
    };

    class OutOfReactRangeTrigger : public FarFromMasterTrigger
    {
    public:
        OutOfReactRangeTrigger(PlayerbotAI* ai) : FarFromMasterTrigger(ai, "out of react range", sPlayerbotAIConfig.reactDistance / 2, 10) {}
    };
}
