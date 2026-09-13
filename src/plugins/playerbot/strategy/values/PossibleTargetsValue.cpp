#include "../../../pchdef.h"
#include "../../playerbot.h"
#include "strategy/values/PossibleTargetsValue.h"

#include "Grids/Notifiers/GridNotifiers.h"
#include "Grids/Notifiers/GridNotifiersImpl.h"
#include "Grids/Cells/CellImpl.h"

using namespace ai;
using namespace Trinity;

void PossibleTargetsValue::FindUnits(list<Unit*> &targets)
{
    AnyUnfriendlyUnitInObjectRangeCheck u_check(bot, bot, range);
    UnitListSearcher<AnyUnfriendlyUnitInObjectRangeCheck> searcher(bot, targets, u_check);
    Cell::VisitAllObjects(bot, searcher, bot->GetMap()->GetVisibilityRange());
}

bool PossibleTargetsValue::AcceptUnit(Unit* unit)
{
    // Unselectable units (event/quest actors, some bosses mid-script) can be
    // unfriendly yet impossible to attack - targeting them only produces an
    // attack -> instant-drop cycle every AI tick.
    //
    // The 3.3.5 flag the upstream bot was written against, UNIT_FLAG_NOT_SELECTABLE
    // (bit 0x02000000), is not declared here: this core names that bit
    // UNIT_FLAG_UNINTERACTIBLE, and answers the whole "can I actually attack it"
    // question with Unit::isTargetableForAttack() - alive, !UNIT_FLAG_NON_ATTACKABLE,
    // !IsUninteractible(), not UNIT_STATE_UNATTACKABLE, not a GM.  Delegating to it
    // keeps the bot in step with the core, which uses the same predicate in
    // NearestAttackableNoTotemUnitInObjectRangeCheck.  checkFakeDeath is off so a
    // creature playing dead stays a candidate, exactly as before.
    return unit->isTargetableForAttack(false) &&
            (unit->IsHostileTo(bot) || (unit->GetLevel() > 1 && !unit->IsFriendlyTo(bot)));
}
