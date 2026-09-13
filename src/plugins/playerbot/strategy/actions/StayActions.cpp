#include "../../../pchdef.h"
#include "../../playerbot.h"
#include "strategy/actions/StayActions.h"
#include "../values/LastMovementValue.h"

using namespace ai;

void StayActionBase::Stay()
{
    AI_VALUE(LastMovement&, "last movement").Set(NULL);

    MotionMaster &mm = *bot->GetMotionMaster();
    if (mm.GetCurrentMovementGeneratorType() == FLIGHT_MOTION_TYPE || bot->IsFlying())
        return;

    mm.Clear();
    mm.MoveIdle();
    // MotionMaster::Clear alone does not stop a running spline - the bot
    // would glide on. Halt it explicitly. (Never blanket-clear unit states
    // here: that kills UNIT_STATE_MELEE_ATTACKING without an AttackStop
    // packet and desyncs the client's attack animation.)
    bot->StopMoving();

    if (!bot->IsStandState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);
}

bool StayAction::Execute(Event event)
{
    Stay();

    return true;
}

bool StayAction::isUseful()
{
    return AI_VALUE2(bool, "moving", "self target");
}
