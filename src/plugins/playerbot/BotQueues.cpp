#include "BotQueues.h"

#include "BotConfig.h"
#include "Player.h"
#include "WorldSession.h"
#include "Unit.h"
#include "Creature.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "Battlegrounds/Battleground.h"
#include "Battlegrounds/BattlegroundMgr.h"
#include "DungeonFinding/LFG.h"
#include "DungeonFinding/LFGMgr.h"
#include "Grids/Notifiers/GridNotifiers.h"
#include "Grids/Cells/Cell.h"
#include "Server/Packets/BattlegroundPackets.h"
#include "Server/Packets/LFGPackets.h"
#include "DB2Stores.h"
#include "Log.h"

#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
    // proposal ids seen per bot (written from the packet hook, read on the
    // world thread - protected)
    std::mutex _proposalMutex;
    std::unordered_map<ObjectGuid, uint32> _proposals;

    struct BattlemasterCheck
    {
        WorldObject const* obj;
        mutable float range;

        BattlemasterCheck(WorldObject const* o, float r) : obj(o), range(r) { }

        bool operator()(Unit* u) const
        {
            if (!u || !u->IsAlive() || !u->IsInWorld())
                return false;
            Creature* c = u->ToCreature();
            if (!c || !c->IsBattleMaster())
                return false;
            if (!obj->IsWithinDist(u, range) || !obj->CanSeeOrDetect(u))
                return false;
            range = obj->GetDistance(u);
            return true;
        }
    };

    struct EnemyCheck
    {
        WorldObject const* obj;
        Player const* bot;
        mutable float range;

        EnemyCheck(WorldObject const* o, Player const* b, float r) : obj(o), bot(b), range(r) { }

        bool operator()(Unit* u) const
        {
            if (!u || !u->IsAlive() || !u->IsInWorld())
                return false;
            if (u == bot)
                return false;
            if (!bot->IsValidAttackTarget(u))
                return false;
            if (!obj->IsWithinDist(u, range) || !obj->CanSeeOrDetect(u))
                return false;
            range = obj->GetDistance(u);
            return true;
        }
    };
}

namespace BotQueues
{
    bool QueueBattleMaster(Player* bot, std::string& reply)
    {
        if (!bot || !bot->IsAlive())
        {
            reply = "I cannot queue right now";
            return false;
        }
        if (bot->InBattleground())
        {
            reply = "I am already inside a battleground";
            return false;
        }
        if (bot->InBattlegroundQueue())
        {
            reply = "I am already queued";
            return false;
        }

        Unit* battlemaster = nullptr;
        BattlemasterCheck check(bot, sBotConfig->SightDistance);
        Trinity::UnitLastSearcher<BattlemasterCheck> checker(bot, battlemaster, check);
        Cell::VisitAllObjects(bot, checker, sBotConfig->SightDistance);

        if (!battlemaster)
        {
            reply = "I need to stand next to a battlemaster";
            return false;
        }

        // every battleground this level bracket can enter
        uint32 const level = bot->GetLevel();
        std::vector<uint32> candidates;
        for (uint32 i = 0; i < sBattlemasterListStore.GetNumRows(); ++i)
            if (BattlemasterListEntry const* entry = sBattlemasterListStore.LookupEntry(i))
                if (entry->MapID[0] >= 0 && entry->MinLevel <= int8(level) && int8(level) <= entry->MaxLevel
                    && !entry->GetFlags().HasFlag(BattlemasterListFlags::InternalOnly))
                    candidates.push_back(entry->ID);

        if (candidates.empty())
        {
            reply = "there is no battleground for my level";
            return false;
        }

        uint32 const listId = candidates[urand(0, uint32(candidates.size()) - 1)];
        BattlegroundQueueTypeId const queueType{ uint16(listId), uint8(BattlegroundQueueIdType::Battleground), false, uint8(0) };

        WorldPackets::Battleground::BattlemasterJoin join{WorldPacket(CMSG_BATTLEMASTER_JOIN)};
        join.PackedBattlegroundQueueTypeID = queueType.GetPacked();
        join.BattlemasterGuid = battlemaster->GetGUID();
        bot->GetSession()->HandleBattlemasterJoinOpcode(join);

        reply = "queued for a battleground";
        return bot->InBattlegroundQueue() || true;
    }

    bool LeaveQueues(Player* bot)
    {
        if (!bot)
            return false;

        if (bot->InBattleground())
        {
            bot->LeaveBattleground(true);
            return true;
        }

        bool left = false;
        for (uint32 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        {
            BattlegroundQueueTypeId const t = bot->GetBattlegroundQueueTypeId(i);
            if (t == BATTLEGROUND_QUEUE_NONE)
                continue;

            WorldPackets::Battleground::BattlefieldPort port{WorldPacket(CMSG_BATTLEFIELD_PORT)};
            port.Ticket.RequesterGuid = bot->GetGUID();
            port.Ticket.Id = i;
            port.Ticket.Type = WorldPackets::LFG::RideType::Battlegrounds;
            port.AcceptedInvite = false;
            bot->GetSession()->HandleBattleFieldPortOpcode(port);
            left = true;
        }
        return left;
    }

    void PumpQueues(Player* bot)
    {
        WorldSession* session = bot->GetSession();
        if (!session)
            return;

        // enter the battleground once the core invites us
        if (!bot->InBattleground())
        {
            for (uint32 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
            {
                BattlegroundQueueTypeId const t = bot->GetBattlegroundQueueTypeId(i);
                if (t == BATTLEGROUND_QUEUE_NONE)
                    continue;
                if (!bot->IsInvitedForBattlegroundQueueType(t))
                    continue;

                WorldPackets::Battleground::BattlefieldPort port{WorldPacket(CMSG_BATTLEFIELD_PORT)};
                port.Ticket.RequesterGuid = bot->GetGUID();
                port.Ticket.Id = i;
                port.Ticket.Type = WorldPackets::LFG::RideType::Battlegrounds;
                port.AcceptedInvite = true;
                session->HandleBattleFieldPortOpcode(port);
                break;
            }
        }

        // dungeon finder: the leader queues, we only answer the proposal
        if (sLFGMgr->GetState(bot->GetGUID()) == lfg::LFG_STATE_PROPOSAL)
        {
            Optional<uint32> proposalId;
            {
                std::lock_guard<std::mutex> lock(_proposalMutex);
                auto it = _proposals.find(bot->GetGUID());
                if (it != _proposals.end())
                    proposalId = it->second;
            }

            if (proposalId)
            {
                WorldPackets::LFG::DFProposalResponse response{WorldPacket(CMSG_DF_PROPOSAL_RESPONSE)};
                response.Ticket.RequesterGuid = bot->GetGUID();
                response.Ticket.Type = WorldPackets::LFG::RideType::Lfg;
                response.ProposalID = *proposalId;
                response.Accepted = true;
                session->HandleLfgProposalResultOpcode(response);

                std::lock_guard<std::mutex> lock(_proposalMutex);
                _proposals.erase(bot->GetGUID());
            }
        }
        else
        {
            std::lock_guard<std::mutex> lock(_proposalMutex);
            _proposals.erase(bot->GetGUID());
        }
    }

    void NotePacket(Player* bot, WorldPacket const* packet)
    {
        if (!bot || !packet || packet->GetOpcode() != SMSG_LFG_PROPOSAL_UPDATE)
            return;

        // mirror of LFGProposalUpdate::Write(): ticket, instance, proposal id
        WorldPacket copy(*packet);
        ObjectGuid requester;
        uint32 id = 0, type = 0, time = 0;
        copy >> requester;
        copy >> id;
        copy >> type;
        copy >> time;
        (void)copy.ReadBit();                       // Unknown925
        (void)requester; (void)type; (void)time;

        uint64 instanceId = 0;
        uint32 proposalId = 0;
        copy >> instanceId;
        copy >> proposalId;

        {
            std::lock_guard<std::mutex> lock(_proposalMutex);
            _proposals[bot->GetGUID()] = proposalId;
        }
    }

    Unit* FindEnemyPlayer(Player* bot, float range)
    {
        Unit* target = nullptr;
        EnemyCheck check(bot, bot, range);
        Trinity::UnitLastSearcher<EnemyCheck> checker(bot, target, check);
        Cell::VisitAllObjects(bot, checker, range);
        return target;
    }
}
