#include "../../../pchdef.h"
#include "../../playerbot.h"
#include "strategy/actions/LfgActions.h"
#include "../../AiFactory.h"
#include "../../PlayerbotAIConfig.h"
#include "../../PlayerbotPackets.h"
#include "../ItemVisitors.h"
#include "../../RandomPlayerbotMgr.h"
#include "DungeonFinding/LFGMgr.h"
#include "DungeonFinding/LFG.h"

using namespace ai;
using namespace lfg;

bool LfgJoinAction::Execute(Event event)
{
    if (!sPlayerbotAIConfig.randomBotJoinLfg)
        return false;

    if (bot->isDead())
        return false;

    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    if (sLFGMgr->GetState(bot->GetGUID()) != LFG_STATE_NONE)
        return false;

    if (bot->IsBeingTeleported())
        return false;

    Map* map = bot->GetMap();
    if (map && map->Instanceable())
        return false;

    return JoinProposal();
}

uint8 LfgJoinAction::GetRoles()
{
    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
    {
        if (ai->IsTank(bot))
            return PLAYER_ROLE_TANK;
        if (ai->IsHeal(bot))
            return PLAYER_ROLE_HEALER;
        else return PLAYER_ROLE_DAMAGE;
    }

    int spec = AiFactory::GetPlayerSpecTab(bot);
    switch (bot->GetClass())
    {
    case CLASS_DRUID:
        if (spec == 2)
            return PLAYER_ROLE_HEALER;
        else if (spec == 1 && bot->GetLevel() >= 40)
            return PLAYER_ROLE_TANK;
        else
            return PLAYER_ROLE_DAMAGE;
        break;
    case CLASS_PALADIN:
        if (spec == 1)
            return PLAYER_ROLE_TANK;
        else if (spec == 0)
            return PLAYER_ROLE_HEALER;
        else
            return PLAYER_ROLE_DAMAGE;
        break;
    case CLASS_PRIEST:
        if (spec != 2)
            return PLAYER_ROLE_HEALER;
        else
            return PLAYER_ROLE_DAMAGE;
        break;
    case CLASS_SHAMAN:
        if (spec == 2)
            return PLAYER_ROLE_HEALER;
        else
            return PLAYER_ROLE_DAMAGE;
        break;
    case CLASS_WARRIOR:
        if (spec == 2)
            return PLAYER_ROLE_TANK;
        else
            return PLAYER_ROLE_DAMAGE;
        break;
    default:
        return PLAYER_ROLE_DAMAGE;
        break;
    }

    return PLAYER_ROLE_DAMAGE;
}

bool LfgJoinAction::SetRoles()
{
    sLFGMgr->SetRoles(bot->GetGUID(), GetRoles());
	return true;
}

bool LfgJoinAction::JoinProposal()
{
    ItemCountByQuality visitor;
    IterateItems(&visitor, ITERATE_ITEMS_IN_EQUIP);
	bool heroic = urand(0, 100) < 50 && (visitor.count[ITEM_QUALITY_EPIC] >= 3 || visitor.count[ITEM_QUALITY_RARE] >= 10) && bot->GetLevel() >= 70;
    bool random = urand(0, 100) < 25;
    bool raid = !heroic && (urand(0, 100) < 50 && visitor.count[ITEM_QUALITY_EPIC] >= 5 && (bot->GetLevel() == 60 || bot->GetLevel() == 70 || bot->GetLevel() == 80));

    LfgDungeonSet list;
    vector<uint32> idx;
    for (uint32 i = 0; i < sLFGDungeonsStore.GetNumRows(); ++i)
    {
        LFGDungeonsEntry const* dungeon = sLFGDungeonsStore.LookupEntry(i);
        if (!dungeon || (dungeon->TypeID != uint8(LFG_TYPE_RANDOM) && dungeon->TypeID != uint8(LFG_TYPE_DUNGEON) && dungeon->TypeID != uint8(LFG_TYPE_HEROIC) &&
                dungeon->TypeID != uint8(LFG_TYPE_RAID)))
            continue;

        int botLevel = (int)bot->GetLevel();
        if (dungeon->MinLevel && botLevel < (int)dungeon->MinLevel)
            continue;

        if (dungeon->MinLevel && botLevel > (int)dungeon->MinLevel + 10)
            continue;

        if (dungeon->MaxLevel && botLevel > (int)dungeon->MaxLevel)
            continue;

        if (heroic && !dungeon->DifficultyID)
            continue;

        if (raid && dungeon->TypeID != uint8(LFG_TYPE_RAID))
            continue;

        if (random && dungeon->TypeID != uint8(LFG_TYPE_RANDOM))
            continue;

        if (!random && !raid && !heroic && dungeon->TypeID != uint8(LFG_TYPE_DUNGEON))
            continue;

        if (!random)
            list.insert(dungeon->ID);
        else
            idx.push_back(dungeon->ID);
    }

    if (list.empty() && !random)
        return false;

    uint8 roles = GetRoles();
    if (random)
	{
        if (idx.empty())
            return false;

        list.insert(idx[urand(0, idx.size() - 1)]);
        sLFGMgr->JoinLfg(bot, roles, list);

        TC_LOG_DEBUG("playerbot",  "Bot {} joined to random dungeon as {}", bot->GetName().c_str(), (uint32)roles);
		return true;
	}
    else if (heroic)
	{
		TC_LOG_DEBUG("playerbot",  "Bot {} joined to heroic dungeon as {}", bot->GetName().c_str(), (uint32)roles);
	}
    else if (raid)
	{
		TC_LOG_DEBUG("playerbot",  "Bot {} joined to raid as {}", bot->GetName().c_str(), (uint32)roles);
	}
    else
	{
		TC_LOG_DEBUG("playerbot",  "Bot {} joined to dungeon as {}", bot->GetName().c_str(), (uint32)roles);
	}

    sLFGMgr->JoinLfg(bot, roles, list);
    return true;
}

bool LfgRoleCheckAction::Execute(Event event)
{
    Group* group = bot->GetGroup();
    if (group)
    {
        uint8 currentRoles = sLFGMgr->GetRoles(bot->GetGUID());
        uint8 newRoles = GetRoles();
        if (currentRoles == newRoles) return false;

        sLFGMgr->UpdateRoleCheck(group->GetGUID(), bot->GetGUID(), newRoles);
        return true;
    }

    return false;
}

bool LfgAcceptAction::Execute(Event event)
{
    uint32 id = AI_VALUE(uint32, "lfg proposal");
    if (id)
    {
        if (urand(0, sPlayerbotAIConfig.GetRandomChangeRange(10.0)))
            return false;

        if (bot->IsInCombat() || bot->isDead() || bot->IsFalling())
        {
            sLFGMgr->UpdateProposal(id, bot->GetGUID(), false);
            return true;
        }

        ai->ChangeStrategy("-grind", BOT_STATE_NON_COMBAT);

        if (sRandomPlayerbotMgr.IsRandomBot(bot) && !bot->GetGroup())
            ai->ChangeStrategy("-grind", BOT_STATE_NON_COMBAT);

        TC_LOG_DEBUG("playerbot",  "Bot {} updated proposal {}", bot->GetName().c_str(), id);
        ai->GetAiObjectContext()->GetValue<uint32>("lfg proposal")->Set(0);
        // Never blanket-clear unit states: on this core that mask wipes
        // MELEE_ATTACKING/CASTING/movement states without the matching
        // packets and desyncs the client. Nothing needs resetting here.
        sLFGMgr->UpdateProposal(id, bot->GetGUID(), true);

        return true;
    }

    packets::LfgProposal proposal;
    if (!packets::ReadLfgProposal(event.getPacket(), proposal))
        return false;
    WorldPackets::LFG::RideTicket const* ticket = sLFGMgr->GetTicket(bot->GetGUID());
    if (!ticket || ticket->Id != proposal.Ticket.Id || ticket->RequesterGuid != proposal.Ticket.RequesterGuid || ticket->Type != proposal.Ticket.Type)
        return false; // group members can legitimately share the leader's ticket
    ai->GetAiObjectContext()->GetValue<uint32>("lfg proposal")->Set(proposal.ProposalID);
    return true;
}

bool LfgLeaveAction::Execute(Event event)
{
    if (sLFGMgr->GetState(bot->GetGUID()) != LFG_STATE_QUEUED)
        return false;

    sLFGMgr->LeaveLfg(bot->GetGUID());
	return true;
}

bool LfgTeleportAction::Execute(Event event)
{
    bool out = false;

    WorldPacket p(event.getPacket());
    if (!p.empty())
    {
        p.rpos(0);
        p >> out;
    }

    // TeleportPlayer resets movement itself; never blanket-clear unit
    // states here (see LfgAcceptAction above).
    sLFGMgr->TeleportPlayer(bot, out);
	return true;
}
