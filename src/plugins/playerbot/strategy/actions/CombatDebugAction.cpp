#include "../../../pchdef.h"
#include "../../playerbot.h"
#include "strategy/actions/CombatDebugAction.h"

using namespace ai;

bool CombatDebugAction::Execute(Event event)
{
    // Deliberately verbose-only: nothing here changes behaviour, it just reads the
    // state the core and the AI disagree about. Replying through the normal master
    // channel means it also works from party/raid chat.
    ai->TellMasterNoFacing(ai->FormatCombatStatus());
    return true;
}
