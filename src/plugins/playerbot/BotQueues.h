/*
 * Playerbot AI - battleground and dungeon queues.
 *
 * Bots join battlegrounds through the real battlemaster queue, walk the
 * enter portal when the core invites them (CMSG_BATTLEFIELD_PORT, exactly
 * what the client's "enter battleground" button sends), fight inside with
 * the normal combat brain (FindEnemyPlayer feeds it BG targets), and leave
 * on order. Dungeon finder: the group leader queues, so bots only have to
 * accept the proposal (parsed from the server's own proposal packet and
 * answered through CMSG_DF_PROPOSAL_RESPONSE).
 */

#ifndef PLAYERBOT_BOT_QUEUES_H
#define PLAYERBOT_BOT_QUEUES_H

#include "Define.h"
#include "Optional.h"

#include <string>

class Player;
class Unit;
class WorldPacket;

namespace BotQueues
{
    /// queue the bot for a level-appropriate battleground ("queue" whisper);
    /// requires a battlemaster nearby, like a real player. Fills reply.
    bool QueueBattleMaster(Player* bot, std::string& reply);

    /// leave all queues / the current battleground ("leave" whisper)
    bool LeaveQueues(Player* bot);

    /// per-tick pump (world thread): answer BG invites and LFG proposals
    void PumpQueues(Player* bot);

    /// remember the proposal id from the server's SMSG_LFG_PROPOSAL_UPDATE
    /// (called from the bot packet hook)
    void NotePacket(Player* bot, WorldPacket const* packet);

    /// nearest enemy we may attack inside a battleground (or world PVP)
    Unit* FindEnemyPlayer(Player* bot, float range);
}

#endif
