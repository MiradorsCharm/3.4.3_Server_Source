/*
 * Playerbot AI - looting.
 *
 * Looting runs through the same server APIs a client-driven player triggers:
 * Player::SendLoot opens the corpse, Player::StoreLootItem stores items and
 * WorldSession::DoLootRelease ends it. No wire parsing - the loot window the
 * server would show the client is irrelevant, the AI acts on the Loot object
 * directly (it is public data on the Creature).
 */

#ifndef PLAYERBOT_BOT_LOOT_H
#define PLAYERBOT_BOT_LOOT_H

#include "Define.h"
#include "ObjectGuid.h"

#include <deque>
#include <cstdint>

class Player;
class Unit;
class Creature;
class BotAI;

class BotLoot
{
public:
    BotLoot(BotAI* ai, Player* bot) : _ai(ai), _bot(bot) { }

    /// queue a dead creature for looting (only bodies the bot may loot)
    void QueueCorpse(Creature* corpse);
    bool HasWork() const { return !_queue.empty() || !_activeGuid.IsEmpty(); }

    /// walk to the next corpse and loot it; call while out of combat
    void Update(uint32 diff);

    void Reset();

private:
    void TryLootActive(Creature* corpse);
    void FinishActive(bool lootedSomething);

    BotAI* _ai;
    Player* _bot;

    std::deque<ObjectGuid> _queue;
    ObjectGuid _activeGuid;
    uint32 _activeTimer = 0;
    uint32 _cooldown = 0;
};

#endif
