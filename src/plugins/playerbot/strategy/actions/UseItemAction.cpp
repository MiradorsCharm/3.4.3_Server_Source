#include "../../../pchdef.h"
#include "../../playerbot.h"
#include "strategy/actions/UseItemAction.h"

using namespace ai;

bool UseItemAction::Execute(Event event)
{
    string name = event.getParam();
    if (name.empty())
        name = getName();

    list<Item*> items = AI_VALUE2(list<Item*>, "inventory items", name);
    list<ObjectGuid> gos = chat->parseGameobjects(name);

    if (gos.empty())
    {
        if (items.size() > 1)
        {
            list<Item*>::iterator i = items.begin();
            Item* itemTarget = *i++;
            Item* item = *i;
            return UseItemOnItem(item, itemTarget);
        }
        else if (!items.empty())
            return UseItemAuto(*items.begin());
    }
    else
    {
        if (items.empty())
            return UseGameObject(*gos.begin());
        else
            return UseItemOnGameObject(*items.begin(), *gos.begin());
    }

    ai->TellMaster("No items (or game objects) available");
    return false;
}

bool UseItemAction::UseGameObject(ObjectGuid guid)
{
    GameObject* go = ai->GetGameObject(guid);
    if (!go || !go->isSpawned())
        return false;

    go->Use(bot);
    ostringstream out; out << "Using " << chat->formatGameobject(go);
    ai->TellMasterNoFacing(out.str());
    return true;
}

bool UseItemAction::UseItemAuto(Item* item)
{
    return UseItem(item, ObjectGuid(), NULL);
}

bool UseItemAction::UseItemOnGameObject(Item* item, ObjectGuid go)
{
    return UseItem(item, go, NULL);
}

bool UseItemAction::UseItemOnItem(Item* item, Item* itemTarget)
{
    return UseItem(item, ObjectGuid(), itemTarget);
}

bool UseItemAction::UseItem(Item* item, ObjectGuid goGuid, Item* itemTarget)
{
    if (bot->CanUseItem(item) != EQUIP_ERR_OK)
        return false;

    if (bot->IsNonMeleeSpellCast(true))
        return false;

    if (bot->IsInCombat() && item->IsPotion() && bot->GetLastPotionId())
        return false;

    // item template can be deleted while the item is still in the bag
    ItemTemplate const* proto = item->GetTemplate();
    if (!proto)
        return false;

    WorldPackets::Spells::UseItem useItem{WorldPacket(CMSG_USE_ITEM)};
    useItem.PackSlot = item->GetBagSlot();
    useItem.Slot = item->GetSlot();
    useItem.CastItem = item->GetGUID();
    useItem.Cast.CastID = ObjectGuid::Create<HighGuid::Cast>(SPELL_CAST_SOURCE_NORMAL, bot->GetMapId(), 0, bot->GetMap()->GenerateLowGuid<HighGuid::Cast>());
    useItem.Cast.SpellID = 0;
    useItem.Cast.Target.Flags = TARGET_FLAG_NONE;

    bool targetSelected = false;
    ostringstream out; out << "Using " << chat->formatItem(proto);
    if (proto->GetMaxStackSize())
    {
        uint32 count = item->GetCount();
        if (count > 1)
            out << " (" << count << " available) ";
        else
            out << " (the last one!)";
    }

    if (!goGuid.IsEmpty())
    {
        GameObject* go = ai->GetGameObject(goGuid);
        if (go && go->isSpawned())
        {
            useItem.Cast.Target.Flags = TARGET_FLAG_GAMEOBJECT;
            useItem.Cast.Target.Unit = goGuid;
            out << " on " << chat->formatGameobject(go);
            targetSelected = true;
        }
    }

    if (itemTarget)
    {
        if (proto->GetClass() == ITEM_CLASS_GEM)
        {
            bool fit = SocketItem(itemTarget, item) || SocketItem(itemTarget, item, true);
            if (!fit)
                ai->TellMaster("Socket does not fit");
            return fit;
        }

        useItem.Cast.Target.Flags = TARGET_FLAG_ITEM;
        useItem.Cast.Target.Item = itemTarget->GetGUID();
        out << " on " << chat->formatItem(itemTarget->GetTemplate());
        targetSelected = true;
    }

    if (uint32 questid = proto->GetStartQuest())
    {
        Quest const* qInfo = sObjectMgr->GetQuestTemplate(questid);
        if (qInfo)
        {
            WorldPackets::Quest::QuestGiverAcceptQuest packet{WorldPacket(CMSG_QUEST_GIVER_ACCEPT_QUEST)};
            packet.QuestGiverGUID = item->GetGUID();
            packet.QuestID = questid;
            packet.StartCheat = false;
            bot->GetSession()->HandleQuestgiverAcceptQuestOpcode(packet);

            ostringstream out; out << "Got quest " << chat->formatQuest(qInfo);
            ai->TellMasterNoFacing(out.str());
            return true;
        }
    }

    MotionMaster &mm = *bot->GetMotionMaster();
    mm.Clear();
    // Stop the spline too: mm.Clear() alone leaves it running, so the bot
    // would glide on and isMoving() below would keep failing. Clearing raw
    // CHASE/FOLLOW states instead only desyncs the motion master.
    bot->StopMoving();

    if (bot->isMoving())
        return false;

    for (uint32 i = 0; i < MAX_ITEM_PROTO_EFFECTS; i++)
    {
        uint32 spellId = ItemSpellId(proto, i);
        if (!spellId)
            continue;

        if (!ai->CanCastSpell(spellId, bot, false))
            continue;

        const SpellInfo* const pSpellInfo = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
        if (!pSpellInfo)
            continue;

        useItem.Cast.SpellID = spellId;

        if (pSpellInfo->Targets & TARGET_FLAG_ITEM)
        {
            Item* itemForSpell = AI_VALUE2(Item*, "item for spell", spellId);
            if (!itemForSpell)
                continue;

            if (itemForSpell->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
                continue;

            if (bot->GetTrader())
            {
                if (selfOnly)
                    return false;

                useItem.Cast.Target.Flags = TARGET_FLAG_TRADE_ITEM;
                useItem.Cast.Target.Item = itemForSpell->GetGUID();
                targetSelected = true;
                out << " on traded item";
            }
            else
            {
                useItem.Cast.Target.Flags = TARGET_FLAG_ITEM;
                useItem.Cast.Target.Item = itemForSpell->GetGUID();
                targetSelected = true;
                out << " on " << chat->formatItem(itemForSpell->GetTemplate());
            }
        }
        else
        {
            useItem.Cast.Target.Flags = TARGET_FLAG_NONE;
            targetSelected = true;
            out << " on self";
        }
        break;
    }

    if (!targetSelected)
        return false;

    if (proto->GetClass() == ITEM_CLASS_CONSUMABLE && proto->GetSubClass() == ITEM_SUBCLASS_FOOD_DRINK)
    {
        if (bot->IsInCombat())
            return false;

        ai->InterruptSpell();
        ai->SetNextCheckDelay(30000);
    }

    ai->TellMasterNoFacing(out.str());
    bot->GetSession()->HandleUseItemOpcode(useItem);
    return true;
}

bool UseItemAction::SocketItem(Item* item, Item* gem, bool replace)
{
    if (!item || !gem)
        return false;

    ItemTemplate const* itemProto = item->GetTemplate();
    ItemTemplate const* gemProto = gem->GetTemplate();
    if (!itemProto || !gemProto)
        return false;

    WorldPackets::Item::SocketGems socketGems{WorldPacket(CMSG_SOCKET_GEMS)};
    socketGems.ItemGuid = item->GetGUID();

    bool fits = false;
    for (uint32 enchant_slot = SOCK_ENCHANTMENT_SLOT; enchant_slot < SOCK_ENCHANTMENT_SLOT + MAX_GEM_SOCKETS; ++enchant_slot)
    {
        uint32 socketIndex = enchant_slot - SOCK_ENCHANTMENT_SLOT;
        uint8 socketColor = itemProto->GetSocketColor(socketIndex);
        GemPropertiesEntry const* gemProperty = sGemPropertiesStore.LookupEntry(gemProto->GetGemProperties());
        if (gemProperty && (gemProperty->Type & socketColor) && !fits)
        {
            uint32 enchant_id = item->GetEnchantmentId(EnchantmentSlot(enchant_slot));
            SpellItemEnchantmentEntry const* enchantEntry = enchant_id ? sSpellItemEnchantmentStore.LookupEntry(enchant_id) : nullptr;

            if (!enchant_id || !enchantEntry || !enchantEntry->GemItemID ||
                (replace && uint32(enchantEntry->GemItemID) != gemProto->GetId()))
            {
                socketGems.GemItem[socketIndex] = gem->GetGUID();
                fits = true;
                continue;
            }
        }

        socketGems.GemItem[socketIndex] = ObjectGuid::Empty;
    }

    if (fits)
    {
        ostringstream out; out << "Socketing " << chat->formatItem(itemProto);
        out << " with " << chat->formatItem(gemProto);
        ai->TellMasterNoFacing(out.str());

        bot->GetSession()->HandleSocketGems(socketGems);
    }
    return fits;
}


bool UseItemAction::isPossible()
{
    return getName() == "use" || AI_VALUE2(uint8, "item count", getName()) > 0;
}

bool UseSpellItemAction::isUseful()
{
    return AI_VALUE2(bool, "spell cast useful", getName());
}
