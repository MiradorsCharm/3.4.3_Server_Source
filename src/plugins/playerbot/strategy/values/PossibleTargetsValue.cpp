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
    return unit->IsAlive() &&
            !unit->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) &&
            !unit->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) &&
            (unit->IsHostileTo(bot) || (unit->GetLevel() > 1 && !unit->IsFriendlyTo(bot)));
}
