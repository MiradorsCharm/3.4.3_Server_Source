#include "../../../pchdef.h"
#include "../../playerbot.h"
#include "../../PlayerbotPackets.h"
#include "../../GuildTaskMgr.h"
#include <limits>
#include "strategy/actions/QueryItemUsageAction.h"
#include "../values/ItemUsageValue.h"
#include "../../../ahbot/AhBot.h"
#include "../../RandomPlayerbotMgr.h"


using namespace ai;


bool QueryItemUsageAction::Execute(Event event)
{
    if (!event.getPacket().empty())
    {
        packets::ItemPush pushed;
        if (!packets::ReadItemPush(event.getPacket(), pushed) || pushed.Player != bot->GetGUID() || pushed.Quantity <= 0)
            return false;
        uint32 itemId = pushed.Item.ItemID;
        if (!itemId && pushed.QuestLogItemID > 0)
            itemId = uint32(pushed.QuestLogItemID);
        ItemTemplate const* item = sObjectMgr->GetItemTemplate(itemId);
        if (!item)
            return false;

        if (!pushed.Pushed && !pushed.Created && sRandomPlayerbotMgr.IsRandomBot(bot))
        {
            uint32 lootAmount = sRandomPlayerbotMgr.GetLootAmount(bot);
            if (Group* group = bot->GetGroup())
            {
                double price = double(pushed.Quantity) * auctionbot.GetSellPrice(item) * sRandomPlayerbotMgr.GetSellMultiplier(bot);
                double total = double(lootAmount) + (std::isfinite(price) && price > 0.0 ? price : 0.0);
                sRandomPlayerbotMgr.SetLootAmount(bot, uint32(std::min(total, double(std::numeric_limits<uint32>::max()))));
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                    if (Player* member = ref->GetSource())
                        if (member != bot)
                            sGuildTaskMgr.CheckItemTask(itemId, uint32(pushed.Quantity), member, bot);
            }
            else if (lootAmount)
                sRandomPlayerbotMgr.SetLootAmount(bot, 0);
        }

        ostringstream out;
        out << chat->formatItem(item, pushed.Quantity);
        if (pushed.Created)
            out << " created";
        else if (pushed.Pushed)
            out << " received";
        ai->TellMaster(out);
        QueryItemUsage(item);
        QueryQuestItem(itemId);
        return true;
    }

    string text = event.getParam();

    ItemIds items = chat->parseItems(text);
    QueryItemsUsage(items);
    return true;
}

bool QueryItemUsageAction::QueryItemUsage(ItemTemplate const *item)
{
    ostringstream out; out << item->GetId();
    ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", out.str());
    switch (usage)
    {
    case ITEM_USAGE_EQUIP:
        ai->TellMaster("Equip");
        return true;
    case ITEM_USAGE_REPLACE:
        ai->TellMaster("Equip (replace)");
        return true;
    case ITEM_USAGE_SKILL:
        ai->TellMaster("Tradeskill");
        return true;
    case ITEM_USAGE_USE:
        ai->TellMaster("Use");
        return true;
    case ITEM_USAGE_GUILD_TASK:
        ai->TellMaster("Guild task");
        return true;
    }

    return false;
}

void QueryItemUsageAction::QueryItemPrice(ItemTemplate const *item)
{
    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return;

    if (item->GetBonding() == BIND_ON_ACQUIRE)
        return;

    list<Item*> items = InventoryAction::parseItems(item->GetDefaultLocaleName());
    if (!items.empty())
    {
        for (list<Item*>::iterator i = items.begin(); i != items.end(); ++i)
        {
            Item* sell = *i;
            ItemTemplate const* proto = sell->GetTemplate();
            if (!proto)
                continue;

            int32 sellPrice = sell->GetCount() * auctionbot.GetSellPrice(proto) * sRandomPlayerbotMgr.GetSellMultiplier(bot);
            ostringstream out;
            out << "Selling " << chat->formatItem(proto, sell->GetCount()) << " for " << chat->formatMoney(sellPrice);
            ai->TellMaster(out.str());
        }
    }

    ostringstream out; out << item->GetId();
    ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", out.str());
    if (usage == ITEM_USAGE_NONE)
        return;

    int32 buyPrice = auctionbot.GetBuyPrice(item) * sRandomPlayerbotMgr.GetBuyMultiplier(bot);
    if (buyPrice)
    {
        ostringstream out;
        out << "Will buy for " << chat->formatMoney(buyPrice);
        ai->TellMaster(out.str());
    }
}

void QueryItemUsageAction::QueryItemsUsage(ItemIds items)
{
    for (ItemIds::iterator i = items.begin(); i != items.end(); i++)
    {
        ItemTemplate const *item = sObjectMgr->GetItemTemplate(*i);
        // skip item ids that have no template in this build's stores rather
        // than dereferencing a null proto
        if (!item)
            continue;
        QueryItemUsage(item);
        QueryQuestItem(*i);
        QueryItemPrice(item);
    }
}

void QueryItemUsageAction::QueryQuestItem(uint32 itemId)
{
    Player *bot = ai->GetBot();
    QuestStatusMap const& questMap = bot->getQuestStatusMap();
    for (QuestStatusMap::const_iterator i = questMap.begin(); i != questMap.end(); i++)
    {
        const Quest *questTemplate = sObjectMgr->GetQuestTemplate( i->first );
        if( !questTemplate )
            continue;

        uint32 questId = questTemplate->GetQuestId();
        QuestStatus status = bot->GetQuestStatus(questId);
        // QUEST_STATE_COMPLETE is a QuestSlotStateMask bit, not a QuestStatus - it
        // only compared equal here by numeric coincidence (both happen to be 1).
        // "Finished but reward not taken yet" is QUEST_STATUS_COMPLETE.
        if (status == QUEST_STATUS_INCOMPLETE || (status == QUEST_STATUS_COMPLETE && !bot->GetQuestRewardStatus(questId)))
        {
            QuestStatusData const& questStatus = i->second;
            QueryQuestItem(itemId, questTemplate, &questStatus);
        }
    }
}


void QueryItemUsageAction::QueryQuestItem(uint32 itemId, const Quest *questTemplate, const QuestStatusData *questStatus)
{
    for (QuestObjective const& objective : questTemplate->GetObjectives())
    {
        if (objective.Type != QUEST_OBJECTIVE_ITEM || uint32(objective.ObjectID) != itemId)
            continue;

        int32 required = objective.Amount;
        if (!required)
            continue;

        int32 available = bot->GetQuestObjectiveData(objective);

        ai->TellMaster(chat->formatQuestObjective(chat->formatQuest(questTemplate), available, required));
    }
}

