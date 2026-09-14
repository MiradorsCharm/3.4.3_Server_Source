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

            // A bot with no usable ranged attack must never back off: its only
            // way to fight is the melee swing the core drives for it. Only a
            // bot that actually has something to shoot/cast belongs at range.
            if (ai->GetBotAttackRange(target) <= 0.0f)
                return false;

            // Centre-to-centre, like every core range test. 11 yd is past the
            // 8-yd minimum range of Shoot/wand with margin; the matching action
            // walks back out toward the bot's own attack range, so trigger and
            // action cannot fight each other over the last yard.
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

    // "Out of spell range" means "further than this bot can actually attack":
    // the longest learned damaging spell plus a usable ranged weapon, computed
    // live from the bot's own spellbook and equipment
    // (PlayerbotAI::GetBotAttackRange). The old version baked a per-class
    // constant into the trigger at construction time - a bot that gained or
    // lost a ranged attack afterwards kept the stale number, and a bot with
    // no usable ranged attack at all was still told to hold at "spell range"
    // and stand there. Both now follow the bot's real reach every tick.
    class EnemyOutOfSpellRangeTrigger : public Trigger
	{
	public:
        EnemyOutOfSpellRangeTrigger(PlayerbotAI* ai) : Trigger(ai, "enemy out of spell range") {}
        virtual bool IsActive()
		{
            Unit* target = AI_VALUE(Unit*, "current target");
            if (!target)
                return false;

            float const range = ai->GetBotAttackRange(target);
            if (range <= 0.0f)
                return false;   // nothing to reach with at range: melee path owns this fight

            // Centre-to-centre 3D distance, the metric Spell::CheckRange()
            // uses - the 2D "distance" value under-reads by the combat
            // reaches and would stop the approach short of real range.
            return bot->GetExactDist(target) > range + sPlayerbotAIConfig.contactDistance;
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
