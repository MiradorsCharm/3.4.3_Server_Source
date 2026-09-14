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
            : ReachTargetAction(ai, "reach spell",
                distance > 0.0f ? distance : ComputeSpellDistance(ai)) {}

        virtual bool isUseful()
        {
            Unit* target = GetTarget();
            if (!target)
                return false;
            // Use live 3D distance here (not the 2D "distance" value the base
            // class uses) so the bot does not decide "I'm in range" because
            // planar distance says 24 yd when the actual 3D gap (on a slope, a
            // ledge, a boat deck or a ramp) is 26 yd and the spell's range
            // check will reject it. GetExactDist is centre-to-centre - the very
            // call Spell::CheckRange() makes - while the "distance" value (and
            // GetDistance, which subtracts both combat reaches) reads up to
            // three yards short of what the core tests against.
            return bot->GetExactDist(target) > (distance + sPlayerbotAIConfig.contactDistance);
        }

        virtual bool isPossible()
        {
            Unit* target = GetTarget();
            if (!target)
                return false;
            return bot->GetExactDist(target) <= (distance + sPlayerbotAIConfig.contactDistance)
                || IsMovingAllowed(target);
        }

    private:
        static float ComputeSpellDistance(PlayerbotAI* ai)
        {
            Player* p = ai->GetBot();
            if (!p)
                return sPlayerbotAIConfig.spellDistance;
            // Hunters / wand-equipped casters can reach 30 yards (auto-shot /
            // wand max); pure casters with a nuke (Shadow Bolt, Frostbolt,
            // Wrath, etc.) also top out at 30-40 yards. Use 30 yd as the
            // approach stop distance so 25 yd is not the hard ceiling the
            // action used to enforce, while still leaving a small safety
            // buffer so the bot does not walk *to* 30 yd and immediately
            // wander out when the target shuffles.
            if (p->GetWeaponForAttack(RANGED_ATTACK, true) != nullptr
                || p->GetClass() == CLASS_WARLOCK || p->GetClass() == CLASS_MAGE
                || p->GetClass() == CLASS_PRIEST || p->GetClass() == CLASS_HUNTER)
                return 28.0f;
            return sPlayerbotAIConfig.spellDistance;
        }
    };
}
