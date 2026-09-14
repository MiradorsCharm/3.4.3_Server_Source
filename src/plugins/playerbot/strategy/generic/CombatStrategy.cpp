#include "../../../pchdef.h"
#include "../../playerbot.h"
#include "strategy/generic/CombatStrategy.h"

using namespace ai;

void CombatStrategy::InitTriggers(list<TriggerNode*> &triggers)
{
    triggers.push_back(new TriggerNode(
        "invalid target",
        NextAction::array(0, new NextAction("drop target", ACTION_HIGH + 9), NULL)));

    // Wired into the base combat strategy (not only RangedCombatStrategy)
    // because priests, shamans and caster druids do not derive from it. The
    // trigger itself decides ranged-ness, so this costs a bot that is not
    // ranged nothing. Without it a ranged bot that got pulled into melee range
    // had "too far" behaviour but no "too close" behaviour at all: it stood in
    // the dead zone unable to shoot, cast or (the auto-repeat spell freezes the
    // combat timers) even swing.
    triggers.push_back(new TriggerNode(
        "enemy inside ranged dead zone",
        NextAction::array(0, new NextAction("back to range", ACTION_HIGH + 2), NULL)));
}
