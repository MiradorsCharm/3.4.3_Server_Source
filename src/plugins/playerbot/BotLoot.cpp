#include "BotLoot.h"

#include "BotAI.h"
#include "BotConfig.h"
#include "BotMovement.h"
#include "Player.h"
#include "Unit.h"
#include "Creature.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "WorldSession.h"
#include "LootMgr.h"
#include "Loot.h"
#include "Log.h"

void BotLoot::QueueCorpse(Creature* corpse)
{
    if (!corpse || !corpse->IsInWorld() || corpse->IsAlive())
        return;
    if (corpse->IsPet() || corpse->IsCritter())
        return;
    if (!corpse->hasLootRecipient())
        return;

    // Only corpses that belong to us (we killed it) or to our group.
    ObjectGuid const& recipient = corpse->GetLootRecipientGUID();
    bool ours = recipient == _bot->GetGUID();
    if (!ours)
        if (Group* group = corpse->GetLootRecipientGroup())
            ours = group->IsMember(_bot->GetGUID());
    if (!ours)
        return;

    if (_queue.size() >= 8)
        return;
    if (std::find(_queue.begin(), _queue.end(), corpse->GetGUID()) != _queue.end())
        return;
    if (_activeGuid == corpse->GetGUID())
        return;

    _queue.push_back(corpse->GetGUID());
}

void BotLoot::Reset()
{
    _queue.clear();
    if (!_activeGuid.IsEmpty())
        FinishActive(false);
}

void BotLoot::FinishActive(bool lootedSomething)
{
    if (_activeGuid.IsEmpty())
        return;
    if (_bot->GetLootGUID() == _activeGuid)
        _bot->GetSession()->DoLootRelease(_activeGuid);
    if (!lootedSomething && _cooldown == 0)
        _cooldown = 1500;
    _activeGuid.Clear();
    _activeTimer = 0;
}

void BotLoot::Update(uint32 diff)
{
    if (_cooldown > diff)
    {
        _cooldown -= diff;
        return;
    }
    _cooldown = 0;

    if (!_activeGuid.IsEmpty())
    {
        Creature* corpse = ObjectAccessor::GetCreature(*_bot, _activeGuid);
        if (!corpse || corpse->IsAlive() || corpse->loot.isLooted())
        {
            FinishActive(false);
            return;
        }

        _activeTimer += diff;
        if (_activeTimer > 15000)
        {
            FinishActive(false);
            return;
        }

        if (_bot->IsWithinDistInMap(corpse, INTERACTION_DISTANCE))
            TryLootActive(corpse);
        else
            _ai->GetMovement().Chase(corpse, INTERACTION_DISTANCE * 0.6f);
        return;
    }

    if (_queue.empty())
        return;

    ObjectGuid guid = _queue.front();
    _queue.pop_front();

    Creature* corpse = ObjectAccessor::GetCreature(*_bot, guid);
    if (!corpse || corpse->IsAlive() || corpse->loot.isLooted())
        return;
    if (_bot->GetExactDist2d(corpse) > sBotConfig->LootDistance)
        return;

    _activeGuid = guid;
    _activeTimer = 0;
}

void BotLoot::TryLootActive(Creature* corpse)
{
    Loot* loot = &corpse->loot;

    // Open the loot the way HandleLootOpcode does for a real client: SendLoot
    // validates distance and recipient rights and sets our looting state.
    if (_bot->GetLootGUID() != corpse->GetGUID())
    {
        _bot->SendLoot(corpse->GetGUID(), LOOT_CORPSE);
        if (_bot->GetLootGUID() != corpse->GetGUID())
        {
            FinishActive(false);
            return;
        }
    }

    if (loot->gold)
    {
        _bot->ModifyMoney(int32(loot->gold));
        loot->gold = 0;
    }

    // Store every item we may take. StoreLootItem repeats the core's own
    // checks (AllowedForPlayer, blocked/rolled items, bag space); a slot that
    // is not ours is a harmless no-op.
    for (uint8 slot = 1; slot <= loot->items.size(); ++slot)
    {
        LootItem* item = loot->LootItemInSlot(slot, _bot);
        if (!item || item->is_looted || item->is_blocked)
            continue;
        _bot->StoreLootItem(corpse->GetGUID(), slot, loot);
    }

    FinishActive(true);
}
