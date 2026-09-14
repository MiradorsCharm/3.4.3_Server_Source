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
        ReachSpellAction(PlayerbotAI* ai, float distance = sPlayerbotAIConfig.spellDistance) : ReachTargetAction(ai, "reach spell", distance) {}
    };
}
