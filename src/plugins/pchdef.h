#ifndef PCHDEF_H_
#define PCHDEF_H_

// Pre-include set for the ported playerbot plugin.
//
// Every plugin translation unit includes this header (directly or through the
// generated PCH force-include), so everything listed here is parsed ONCE per
// build instead of once per source file.
//
// Rule of thumb (same as src/server/game/PrecompiledHeaders/gamePCH.h): only
// put *rarely modified* headers in here.  The bot's own headers are tiny
// (~1.8k lines in total) and change constantly, so they are deliberately NOT
// part of the PCH - keeping them out means editing a bot header does not
// invalidate the PCH and force a full rebuild of the 258 plugin sources.
//
// Measured cost of what used to be re-parsed by every bot source file:
// playerbot.h pulls in 189 core headers / ~74k lines, versus ~1.8k lines of
// playerbot headers.  Precompiling the core set is where the win is.

// Original pre-include set (mirrors the old src/plugins/pch/pch.h the
// 2016-era bot sources were written against).  Kept for compatibility - all of
// these also come in through the game PCH below.
#include "Common.h"
#include "Entities/Object/ObjectDefines.h"
#include "Entities/Object/ObjectGuid.h"
#include "Server/WorldPacket.h"
#include "Log.h"
#include "Database/DatabaseEnv.h"

#include <map>
#include <list>
#include <set>
#include <vector>
#include <string>
#include <sstream>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <memory>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <stack>
#include <deque>

// --- core: the exact set the game library precompiles ------------------------
// Single source of truth - if the core needs another header in its PCH, the
// bots get it too.
#include "PrecompiledHeaders/gamePCH.h"

// --- core headers the bots need on top of the game PCH -----------------------
#include "Cache/CharacterCache.h"
#include "Spells/Spell.h"
#include "Spells/SpellInfo.h"
#include "Spells/SpellHistory.h"
#include "Spells/Auras/SpellAuras.h"
#include "Spells/Auras/SpellAuraEffects.h"
#include "Loot/LootMgr.h"
#include "Loot/Loot.h"
#include "Entities/Creature/GossipDef.h"
#include "Entities/Creature/Trainer.h"
#include "Chat/Chat.h"
#include "Globals/ObjectAccessor.h"
#include "Entities/Unit/Unit.h"
#include "Entities/Unit/CharmInfo.h"
#include "Entities/Item/Item.h"
#include "Entities/Item/ItemTemplate.h"
#include "Entities/Item/Container/Bag.h"
#include "Entities/GameObject/GameObject.h"
#include "Entities/Pet/Pet.h"
#include "Entities/Corpse/Corpse.h"
#include "Entities/Player/TradeData.h"
#include "Maps/MapManager.h"
#include "Mails/Mail.h"
#include "Miscellaneous/SharedDefines.h"
#include "Movement/MotionMaster.h"
#include "Guilds/Guild.h"
#include "Guilds/GuildMgr.h"
#include "Groups/Group.h"
#include "Accounts/AccountMgr.h"
#include "Grids/Notifiers/GridNotifiers.h"
#include "Grids/Notifiers/GridNotifiersImpl.h"
#include "Grids/Cells/CellImpl.h"
#include "AI/CreatureAI.h"
#include "Server/WorldPacket.h"

// --- client packet classes the bots feed into the session handlers -----------
#include "Server/Packets/AreaTriggerPackets.h"
#include "Server/Packets/CharacterPackets.h"
#include "Server/Packets/ChatPackets.h"
#include "Server/Packets/DuelPackets.h"
#include "Server/Packets/EquipmentSetPackets.h"
#include "Server/Packets/GuildPackets.h"
#include "Server/Packets/ItemPackets.h"
#include "Server/Packets/LootPackets.h"
#include "Server/Packets/MailPackets.h"
#include "Server/Packets/MiscPackets.h"
#include "Server/Packets/MovementPackets.h"
#include "Server/Packets/NPCPackets.h"
#include "Server/Packets/PartyPackets.h"
#include "Server/Packets/QuestPackets.h"
#include "Server/Packets/SpellPackets.h"
#include "Server/Packets/TaxiPackets.h"
#include "Server/Packets/TradePackets.h"

#endif /* PCHDEF_H_ */
