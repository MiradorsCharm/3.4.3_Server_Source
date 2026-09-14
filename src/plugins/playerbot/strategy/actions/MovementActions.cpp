#include "../../../pchdef.h"
#include "../../playerbot.h"
#include "../values/LastMovementValue.h"
#include "strategy/actions/MovementActions.h"
#include "Movement/MotionMaster.h"
#include "Movement/MovementGenerator.h"
#include "Movement/Spline/MoveSpline.h"
#include "../../FleeManager.h"
#include "../../LootObjectStack.h"
#include "../../PlayerbotAIConfig.h"
#include "../../CombatDiag.h"

using namespace ai;

bool MovementAction::MoveNear(uint32 mapId, float x, float y, float z, float distance)
{
    float angle = GetFollowAngle();
    return MoveTo(mapId, x + cos(angle) * distance, y + sin(angle) * distance, z);
}

bool MovementAction::MoveNear(WorldObject* target, float distance)
{
    if (!target)
        return false;

    distance += target->GetCombatReach() / 2.0f;

    float followAngle = GetFollowAngle();
    for (float angle = followAngle; angle <= followAngle + 2 * M_PI; angle += M_PI / 4)
    {
        bool moved = MoveTo(target->GetMapId(),
            target->GetPositionX() + cos(angle) * distance,
            target->GetPositionY()+ sin(angle) * distance,
            target->GetPositionZ());
        if (moved)
            return true;
    }
    return false;
}

bool MovementAction::MoveTo(uint32 mapId, float x, float y, float z)
{
    bot->UpdateGroundPositionZ(x, y, z);
    if (!IsMovingAllowed(mapId, x, y, z))
        return false;

    float distance = bot->GetDistance2d(x, y);
    if (distance > sPlayerbotAIConfig.contactDistance)
    {
        // Reach/follow actions run every AI tick: tearing down and relaunching
        // the spline each time makes movement stutter and spams spline packets
        // to everyone nearby. If the live spline is already taking us (almost)
        // to THIS destination (not some previous target that moved away), let
        // it run and just wait for it. The old check used to compare against
        // the spline's FinalDestination() with a stale snapshot from the last
        // chase, so a target that walked across the clearing would leave the
        // bot "following" a spline that ended 10 yards short forever - exactly
        // the "gap not closing" case the combat watchdog reports.
        bool splineAlreadyOnTarget = false;
        if (!bot->movespline->Finalized())
        {
            auto dest = bot->movespline->FinalDestination();
            float dx = dest.x - x, dy = dest.y - y;
            float dz = dest.z - z;
            if (dx * dx + dy * dy < 2.0f * 2.0f && dz * dz < 2.0f * 2.0f)
                splineAlreadyOnTarget = true;
        }

        if (splineAlreadyOnTarget)
        {
            WaitForReach(distance);
            return true;
        }

        if (bot->IsSitState())
            bot->SetStandState(UNIT_STAND_STATE_STAND);

        if (bot->IsNonMeleeSpellCast(true))
        {
            bot->CastStop();
            ai->InterruptSpell();
        }

        bool generatePath = !bot->HasAuraType(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED) &&
                !bot->IsFlying() && !bot->IsUnderWater();
        MotionMaster &mm = *bot->GetMotionMaster();
        mm.Clear();
        bot->StopMoving();

        float botZ = bot->GetPositionZ();
        if (z - botZ > 0.5f && bot->GetDistance2d(x, y) <= 5.0f)
        {
            float speed = bot->GetSpeed(MOVE_RUN);
            mm.MoveJump(x, y, botZ + 0.5f, speed, speed, 1);
        }
        else
            mm.MovePoint(mapId, x, y, z, generatePath);

        WaitForReach(distance);
        AI_VALUE(LastMovement&, "last movement").Set(x, y, z, bot->GetOrientation());
        return true;
    }

    return false;
}

bool MovementAction::MoveTo(Unit* target, float distance)
{
    if (!IsMovingAllowed(target))
        return false;

    float bx = bot->GetPositionX();
    float by = bot->GetPositionY();
    float bz = bot->GetPositionZ();

    float tx = target->GetPositionX();
    float ty = target->GetPositionY();
    float tz = target->GetPositionZ();

    float distanceToTarget = bot->GetDistance2d(target);
    float angle = bot->GetAbsoluteAngle(target);
    float needToGo = distanceToTarget - distance;

    float maxDistance = sPlayerbotAIConfig.spellDistance;
    if (needToGo > 0 && needToGo > maxDistance)
        needToGo = maxDistance;
    else if (needToGo < 0 && needToGo < -maxDistance)
        needToGo = -maxDistance;

    float dx = cos(angle) * needToGo + bx;
    float dy = sin(angle) * needToGo + by;

    return MoveTo(target->GetMapId(), dx, dy, tz);
}

float MovementAction::GetFollowAngle()
{
    Player* master = GetMaster();
    Group* group = master ? master->GetGroup() : bot->GetGroup();
    if (!group || group->GetMembersCount() < 2)
        return 0.0f;

    int index = 1;
    for (GroupReference *ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        if( ref->GetSource() == master)
            continue;

        if( ref->GetSource() == bot)
            return 2 * M_PI / (group->GetMembersCount() -1) * index;

        index++;
    }
    return 0;
}

bool MovementAction::IsMovingAllowed(Unit* target)
{
    if (!target)
        return false;

    if (bot->GetMapId() != target->GetMapId())
        return false;

    float distance = bot->GetDistance(target);
    if (distance > sPlayerbotAIConfig.reactDistance)
        return false;

    return IsMovingAllowed();
}

bool MovementAction::IsMovingAllowed(uint32 mapId, float x, float y, float z)
{
    float distance = bot->GetDistance(x, y, z);
    if (distance > sPlayerbotAIConfig.reactDistance)
        return false;

    return IsMovingAllowed();
}

bool MovementAction::IsMovingAllowed()
{
    if (bot->IsFrozen() || bot->IsPolymorphed() ||
			(bot->isDead() && !bot->HasPlayerFlag(PLAYER_FLAGS_GHOST)) ||
            bot->IsBeingTeleported() ||
            bot->HasUnitState(UNIT_STATE_ROOT) ||
            bot->HasAuraType(SPELL_AURA_MOD_CONFUSE) || bot->IsCharmed() ||
            bot->HasAuraType(SPELL_AURA_MOD_STUN) || bot->IsFlying())
        return false;

    MotionMaster &mm = *bot->GetMotionMaster();
    return mm.GetCurrentMovementGeneratorType() != FLIGHT_MOTION_TYPE;
}

bool MovementAction::IsInMeleeRange(Unit* target) const
{
    // Out-of-line on purpose: the header is included by translation units that have
    // not pulled in Unit.h yet, and this needs the complete Player.
    return target != nullptr && bot->IsWithinMeleeRange(target);
}

float MovementAction::GetMeleeApproachDistance(Unit* target) const
{
    if (!target)
        return sPlayerbotAIConfig.meleeDistance;

    return ComputeMeleeStopDistance(bot->GetMeleeRange(target), bot->GetDistance(target),
            bot->GetPositionZ() - target->GetPositionZ(), sPlayerbotAIConfig.meleeDistance);
}

bool MovementAction::ApproachForMelee(Unit* target)
{
    if (!target)
        return false;

    if (IsInMeleeRange(target))
        return true;

    if (!IsMovingAllowed(target))
        return false;

    return MoveTo(target, GetMeleeApproachDistance(target));
}

bool MovementAction::Follow(Unit* target, float distance)
{
    return Follow(target, distance, GetFollowAngle());
}

bool MovementAction::Follow(Unit* target, float distance, float angle)
{
    MotionMaster &mm = *bot->GetMotionMaster();

    if (!target)
        return false;

    if (bot->GetDistance2d(target->GetPositionX(), target->GetPositionY()) <= sPlayerbotAIConfig.sightDistance &&
            abs(bot->GetPositionZ() - target->GetPositionZ()) >= sPlayerbotAIConfig.spellDistance)
    {
        mm.Clear();
        // A live spline would overwrite the snap on the next update.
        bot->StopMoving();
        float x = bot->GetPositionX(), y = bot->GetPositionY(), z = target->GetPositionZ();
        // Map 0 (Eastern Kingdoms) is a valid map id, so it must not gate the
        // cross-map branch - comparing the maps is the whole check.
        if (bot->GetMapId() != target->GetMapId())
        {
            bot->TeleportTo(target->GetMapId(), x, y, z, bot->GetOrientation());
        }
        else
        {
            bot->Relocate(x, y, z, bot->GetOrientation());
        }
        AI_VALUE(LastMovement&, "last movement").Set(target);
        return true;
    }

    // Already glued to this target: MoveFollow would tear down and rebuild
    // the follow generator (and re-path) every AI tick. Let it run. (Same map
    // only - a cross-map target needs a fresh follow/teleport, not this.)
    if (bot->GetMapId() == target->GetMapId() &&
        mm.GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE &&
        AI_VALUE(LastMovement&, "last movement").lastFollow == target)
        return true;

    if (!IsMovingAllowed(target))
        return false;

    if (target->IsFriendlyTo(bot) && bot->IsMounted() && AI_VALUE(list<ObjectGuid>, "possible targets").empty())
        distance += angle;

    if (bot->GetDistance2d(target) <= sPlayerbotAIConfig.followDistance)
        return false;

    if (bot->IsSitState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    if (bot->IsNonMeleeSpellCast(true))
    {
        bot->CastStop();
        ai->InterruptSpell();
    }

    mm.MoveFollow(target, distance, angle);

    AI_VALUE(LastMovement&, "last movement").Set(target);
    return true;
}

void MovementAction::WaitForReach(float distance)
{
    float delay = 1000.0f * distance / bot->GetSpeed(MOVE_RUN) + sPlayerbotAIConfig.reactDelay;

    if (delay > sPlayerbotAIConfig.maxWaitForMove)
        delay = sPlayerbotAIConfig.maxWaitForMove;

    Unit* target = *ai->GetAiObjectContext()->GetValue<Unit*>("current target");
    Unit* player = *ai->GetAiObjectContext()->GetValue<Unit*>("enemy player target");
    if ((player || target) && delay > sPlayerbotAIConfig.globalCoolDown)
        delay = sPlayerbotAIConfig.globalCoolDown;

    ai->SetNextCheckDelay((uint32)delay);
}

bool MovementAction::Flee(Unit *target)
{
    Player* master = GetMaster();
    if (!target)
        target = master;

    if (!target)
        return false;

    if (!sPlayerbotAIConfig.fleeingEnabled)
        return false;

    if (!IsMovingAllowed())
        return false;

    FleeManager manager(bot, sPlayerbotAIConfig.fleeDistance, bot->GetAbsoluteAngle(target) + M_PI);

    float rx, ry, rz;
    if (!manager.CalculateDestination(&rx, &ry, &rz))
        return false;

    return MoveTo(target->GetMapId(), rx, ry, rz);
}

bool FleeAction::Execute(Event event)
{
    return Flee(AI_VALUE(Unit*, "current target"));
}

bool FleeAction::isUseful()
{
    return AI_VALUE(uint8, "attacker count") > 0 &&
            AI_VALUE2(float, "distance", "current target") <= sPlayerbotAIConfig.tooCloseDistance &&
            AI_VALUE2(uint8, "speed", "current target") < 100;
}

bool RunAwayAction::Execute(Event event)
{
    return Flee(AI_VALUE(Unit*, "master target"));
}

bool MoveRandomAction::Execute(Event event)
{
    WorldObject* target = NULL;

    if (!(rand() % 3))
    {
        list<ObjectGuid> npcs = AI_VALUE(list<ObjectGuid>, "nearest npcs");
        for (list<ObjectGuid>::iterator i = npcs.begin(); i != npcs.end(); i++)
        {
            target = ai->GetUnit(*i);

            if (target && bot->GetDistance(target) > sPlayerbotAIConfig.tooCloseDistance)
                break;
        }
    }

    if (!target || !(rand() % 3))
    {
        list<ObjectGuid> gos = AI_VALUE(list<ObjectGuid>, "nearest game objects");
        for (list<ObjectGuid>::iterator i = gos.begin(); i != gos.end(); i++)
        {
            target = ai->GetGameObject(*i);

            if (target && bot->GetDistance(target) > sPlayerbotAIConfig.tooCloseDistance)
                break;
        }
    }

    float distance = sPlayerbotAIConfig.tooCloseDistance + sPlayerbotAIConfig.grindDistance * urand(3, 10) / 10.0f;

    Map* map = bot->GetMap();
    if (target)
    {
        float x = target->GetPositionX();
        float y = target->GetPositionY();
        float z = target->GetPositionZ();
        if (!map->IsInWater(bot->GetPhaseShift(), x, y, z))
        {
            return MoveNear(target);
        }
    }

    for (int i = 0; i < 10; ++i)
    {
        float x = bot->GetPositionX();
        float y = bot->GetPositionY();
        float z = bot->GetPositionZ();
        x += urand(0, distance) - distance / 2;
        y += urand(0, distance) - distance / 2;
        bot->UpdateGroundPositionZ(x, y, z);

        if (map->IsInWater(bot->GetPhaseShift(), x, y, z))
            continue;

        bool moved = MoveNear(bot->GetMapId(), x, y, z);
        if (moved)
            return true;
    }

    return false;
}

bool MoveToLootAction::Execute(Event event)
{
    LootObject loot = AI_VALUE(LootObject, "loot target");
    if (!loot.IsLootPossible(bot))
        return false;

    return MoveNear(loot.GetWorldObject(bot));
}

bool MoveOutOfEnemyContactAction::Execute(Event event)
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return false;

    return MoveNear(target, sPlayerbotAIConfig.meleeDistance);
}

bool MoveOutOfEnemyContactAction::isUseful()
{
    return AI_VALUE2(float, "distance", "current target") < (sPlayerbotAIConfig.meleeDistance + sPlayerbotAIConfig.contactDistance);
}

bool SetFacingTargetAction::Execute(Event event)
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return false;

    bot->SetFacingTo(bot->GetAbsoluteAngle(target));
    ai->SetNextCheckDelay(sPlayerbotAIConfig.globalCoolDown);
    return true;
}

bool SetFacingTargetAction::isUseful()
{
    return !AI_VALUE2(bool, "facing", "current target");
}
