#pragma once

#include "../Action.h"
#include "strategy/actions/MovementActions.h"
#include "../../PlayerbotAIConfig.h"

namespace ai
{
    class ReachTargetAction : public MovementAction
    {
    public:
        ReachTargetAction(PlayerbotAI* ai, string name, float distance) : MovementAction(ai, name)
		{
            this->distance = distance;
        }
        virtual bool Execute(Event event)
		{
			return MoveTo(AI_VALUE(Unit*, "current target"), distance);
        }
        virtual bool isUseful()
		{
            return AI_VALUE2(float, "distance", "current target") > (distance + sPlayerbotAIConfig.contactDistance);
        }
        virtual string GetTargetName() { return "current target"; }

    protected:
        float distance;
    };

    class CastReachTargetSpellAction : public CastSpellAction
    {
    public:
        CastReachTargetSpellAction(PlayerbotAI* ai, string spell, float distance) : CastSpellAction(ai, spell)
		{
            this->distance = distance;
        }
		virtual bool isUseful()
		{
			return AI_VALUE2(float, "distance", "current target") > (distance + sPlayerbotAIConfig.contactDistance);
		}

    protected:
        float distance;
    };

    class ReachMeleeAction : public ReachTargetAction
	{
    public:
        ReachMeleeAction(PlayerbotAI* ai) : ReachTargetAction(ai, "reach melee", sPlayerbotAIConfig.meleeDistance) {}

        // Melee is deliberately not measured with the 2D "distance" value the base
        // class uses: the core only lets a swing land inside IsWithinMeleeRange()
        // (3D and combat-reach based), so a bot that stops at "meleeDistance" of
        // planar distance can still be locked out of its own attack - on a slope, a
        // ledge, a ramp or a boat. Keep walking until the swing test passes, and
        // plant inside that envelope instead of exactly on its edge.
        virtual bool isUseful()
        {
            return !IsInMeleeRange(GetTarget());
        }

        virtual bool Execute(Event event)
        {
            Unit* target = GetTarget();
            if (!target)
                return false;

            return MoveTo(target, GetMeleeApproachDistance(target));
        }

        virtual bool isPossible()
        {
            // Out of reach *and* not allowed to move is not something to retry every
            // tick; report it as impossible so the alternatives and the abandon path
            // run instead of the bot fidgeting in place.
            Unit* target = GetTarget();
            return IsInMeleeRange(target) || IsMovingAllowed(target);
        }
    };

    class ReachSpellAction : public ReachTargetAction
	{
	public:
        ReachSpellAction(PlayerbotAI* ai, float distance = 0.0f)
            : ReachTargetAction(ai, "reach spell", distance) {}

        // How far this bot can actually attack from, computed from its own
        // spellbook and equipped weapon every time - never a per-class
        // constant. A bot with no usable ranged attack returns 0 and does not
        // "reach spell" at all: the melee path (reach melee) handles it.
        float GetAttackRange()
        {
            if (distance > 0.0f)
                return distance;   // explicit pull-style distance, keep it

            Unit* target = GetTarget();
            return ai->GetBotAttackRange(target);
        }

        virtual bool isUseful()
		{
            Unit* target = GetTarget();
            if (!target)
                return false;

            float const range = GetAttackRange();
            if (range <= 0.0f)
                return false;

            // Use live 3D distance here (not the 2D "distance" value the base
            // class uses) so the bot does not decide "I'm in range" because
            // planar distance says 24 yd when the actual 3D gap (on a slope, a
            // ledge, a boat deck or a ramp) is 26 yd and the spell's range
            // check will reject it. GetExactDist is centre-to-centre - the very
            // call Spell::CheckRange() makes - while the "distance" value (and
            // GetDistance, which subtracts both combat reaches) reads up to
            // three yards short of what the core tests against.
            return bot->GetExactDist(target) > (range + sPlayerbotAIConfig.contactDistance);
        }

        virtual bool isPossible()
        {
            Unit* target = GetTarget();
            if (!target)
                return false;

            float const range = GetAttackRange();
            if (range <= 0.0f)
                return false;

            return bot->GetExactDist(target) <= (range + sPlayerbotAIConfig.contactDistance)
                || IsMovingAllowed(target);
        }

        virtual bool Execute(Event event)
        {
            Unit* target = GetTarget();
            if (!target)
                return false;

            float const range = GetAttackRange();
            if (range <= 0.0f)
                return false;

            // Stop a couple of yards inside the attack envelope so target
            // movement does not instantly push the bot out of range again.
            float const stop = std::max(range - 2.0f, sPlayerbotAIConfig.meleeDistance);
            return MoveTo(target, stop);
        }
    };
}
