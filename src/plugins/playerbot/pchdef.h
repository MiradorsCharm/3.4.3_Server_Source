/*
 * Pre-include set (PCH source) for the playerbot plugin.
 *
 * Only rarely modified core headers belong here (same rule as
 * src/server/game/PrecompiledHeaders/gamePCH.h). The bot's own headers are
 * not part of it, so editing a bot header never invalidates the PCH.
 */

#ifndef PCHDEF_H_
#define PCHDEF_H_

#include "Common.h"
#include "Define.h"

#include "Log.h"
#include "Config.h"

#include "ObjectGuid.h"
#include "SharedDefines.h"
#include "UnitDefines.h"

#include "Timer.h"

#endif
