#include "BotInteract.h"

#include "BotAI.h"
#include "BotConfig.h"
#include "Player.h"
#include "WorldSession.h"
#include "Unit.h"
#include "Creature.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Container/Bag.h"
#include "Entities/Player/TradeData.h"
#include "DatabaseEnv.h"
#include "Guilds/Guild.h"
#include "Guilds/GuildMgr.h"
#include "Grids/Notifiers/GridNotifiers.h"
#include "Grids/Cells/Cell.h"
#include "Server/Packets/DuelPackets.h"
#include "Server/Packets/TradePackets.h"
#include "Server/Packets/GuildPackets.h"
#include "Server/Packets/ItemPackets.h"
#include "Server/Packets/AuctionHousePackets.h"
#include "Server/Packets/MailPackets.h"
#include "Mail.h"
#include "GameObject.h"
#include "QuestDef.h"
#include "ObjectMgr.h"
#include "Server/Packets/QuestPackets.h"
#include "Opcodes.h"
#include "AuctionHouse/AuctionHouseMgr.h"
#include "Map.h"
#include "Log.h"

#include <ctime>
#include <unordered_map>
#include <unordered_set>

namespace
{
    // per-bot throttle for the mail sweep
    std::unordered_map<ObjectGuid, time_t>& MailSweepClock()
    {
        static std::unordered_map<ObjectGuid, time_t> map;
        return map;
    }

    struct MailboxCheck
    {
        WorldObject const* obj;
        mutable float range;

        MailboxCheck(WorldObject const* o, float r) : obj(o), range(r) { }

        bool operator()(GameObject* go) const
        {
            if (!go || !go->IsInWorld() || go->GetGoType() != GAMEOBJECT_TYPE_MAILBOX)
                return false;
            if (!obj->IsWithinDist(go, range) || !obj->CanSeeOrDetect(go))
                return false;
            range = obj->GetDistance(go);
            return true;
        }
    };

    GameObject* FindMailbox(Player* bot)
    {
        GameObject* result = nullptr;
        MailboxCheck check(bot, 5.0f);
        Trinity::GameObjectLastSearcher<MailboxCheck> searcher(bot, result, check);
        Cell::VisitAllObjects(bot, searcher, 5.0f);
        return result;
    }

    /// collect attachments + money from every delivered, non-COD mail while
    /// standing at a mailbox, then delete the mail - what a player does when
    /// they click through their inbox, done on a 60s cadence
    void SweepMail(Player* bot)
    {
        time_t const now = time(nullptr);
        auto& clock = MailSweepClock();
        auto it = clock.find(bot->GetGUID());
        if (it != clock.end() && it->second + 60 > now)
            return;
        clock[bot->GetGUID()] = now;

        bool hasContent = false;
        for (Mail const* mail : bot->GetMails())
        {
            if (!mail)
                continue;
            if (mail->deliver_time > now || mail->COD)
                continue;                           // undelivered or cash-on-demand
            if (mail->money || mail->HasItems())
            {
                hasContent = true;
                break;
            }
        }
        if (!hasContent)
            return;

        GameObject* mailbox = FindMailbox(bot);
        if (!mailbox)
            return;                                 // walk to a mailbox first

        WorldSession* session = bot->GetSession();
        std::vector<Mail*> mails;
        for (Mail* mail : bot->GetMails())
            if (mail && mail->deliver_time <= now && !mail->COD && (mail->money || mail->HasItems()))
                mails.push_back(mail);

        for (Mail* mail : mails)
        {
            for (MailItemInfo const& item : mail->items)
            {
                WorldPackets::Mail::MailTakeItem take{WorldPacket(CMSG_MAIL_TAKE_ITEM)};
                take.Mailbox = mailbox->GetGUID();
                take.MailID = mail->messageID;
                take.AttachID = item.item_guid;
                session->HandleMailTakeItem(take);
            }

            if (mail->money)
            {
                WorldPackets::Mail::MailTakeMoney money{WorldPacket(CMSG_MAIL_TAKE_MONEY)};
                money.Mailbox = mailbox->GetGUID();
                money.MailID = mail->messageID;
                money.Money = mail->money;
                session->HandleMailTakeMoney(money);
            }

            WorldPackets::Mail::MailDelete del{WorldPacket(CMSG_MAIL_DELETE)};
            del.MailID = mail->messageID;
            del.DeleteReason = 0;
            session->HandleMailDelete(del);
        }
    }

    // trades the bot already opened its half of (reset when the trade ends)
    std::unordered_set<ObjectGuid>& BegunTrades()
    {
        static std::unordered_set<ObjectGuid> set;
        return set;
    }

    struct NpcInRangeCheck
    {
        WorldObject const* obj;
        mutable float range;

        NpcInRangeCheck(WorldObject const* o, float r) : obj(o), range(r) { }

        bool operator()(Unit* u) const
        {
            if (!u || !u->IsAlive() || !u->IsInWorld())
                return false;
            Creature* c = u->ToCreature();
            if (!c)
                return false;
            if (!c->HasNpcFlag(UNIT_NPC_FLAG_AUCTIONEER) && !c->HasNpcFlag(UNIT_NPC_FLAG_BATTLEMASTER)
                && !c->HasNpcFlag(UNIT_NPC_FLAG_VENDOR))
                return false;
            if (!obj->IsWithinDist(u, range) || !obj->CanSeeOrDetect(u))
                return false;
            range = obj->GetDistance(u);
            return true;
        }
    };

    Creature* FindNearNpc(Player* bot, NPCFlags npcFlag, float range)
    {
        Unit* found = nullptr;
        NpcInRangeCheck check(bot, range);
        Trinity::UnitLastSearcher<NpcInRangeCheck> checker(bot, found, check);
        Cell::VisitAllObjects(bot, checker, sBotConfig->SightDistance);
        if (!found)
            return nullptr;
        Creature* creature = found->ToCreature();
        return creature && creature->HasNpcFlag(npcFlag) ? creature : nullptr;
    }

    Creature* FindQuestGiver(Player* bot)
    {
        Creature* giver = FindNearNpc(bot, UNIT_NPC_FLAG_QUESTGIVER, sBotConfig->SightDistance);
        if (!giver)
            return nullptr;
        return bot->GetNPCIfCanInteractWith(giver->GetGUID(), UNIT_NPC_FLAG_QUESTGIVER, UNIT_NPC_FLAG_2_NONE);
    }
}

namespace BotInteract
{
    void PumpSession(Player* bot)
    {
        WorldSession* session = bot->GetSession();
        if (!session)
            return;

        SweepMail(bot);

        BotAI* ai = bot->GetBotAI();

        // --- duel requests: accept from trusted initiators ------------------
        if (bot->duel && bot->duel->State == DUEL_STATE_CHALLENGED && bot->duel->Initiator != bot)
        {
            if (ai && ai->AcceptsCommandsFrom(bot->duel->Initiator))
            {
                WorldPackets::Duel::DuelResponse response{WorldPacket(CMSG_DUEL_RESPONSE)};
                response.ArbiterGUID = bot->m_playerData->DuelArbiter;
                response.Accepted = true;
                response.Forfeited = false;
                session->HandleDuelResponseOpcode(response);
            }
        }

        // --- trade: open our half, then accept once the trader accepted ----
        if (bot->GetTradeData())
        {
            Player* trader = bot->GetTradeData()->GetTrader();
            if (trader && ai && ai->AcceptsCommandsFrom(trader))
            {
                auto& begun = BegunTrades();
                if (!begun.count(bot->GetGUID()))
                {
                    begun.insert(bot->GetGUID());
                    WorldPackets::Trade::BeginTrade begin{WorldPacket(CMSG_BEGIN_TRADE)};
                    session->HandleBeginTradeOpcode(begin);
                }

                TradeData* traderTrade = trader->GetTradeData();
                if (traderTrade && traderTrade->IsAccepted() && !bot->GetTradeData()->IsAccepted())
                {
                    WorldPackets::Trade::AcceptTrade accept{WorldPacket(CMSG_ACCEPT_TRADE)};
                    accept.StateIndex = bot->GetTradeData()->GetServerStateIndex();
                    session->HandleAcceptTradeOpcode(accept);
                }
            }
        }
        else
            BegunTrades().erase(bot->GetGUID());
    }

    bool JoinMastersGuild(Player* bot, Player* master)
    {
        if (!bot || !master || bot->GetGuildId())
            return false;

        Guild* guild = master->GetGuild();
        if (!guild)
            return false;

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        if (!guild->AddMember(trans, bot->GetGUID()))
            return false;
        CharacterDatabase.CommitTransaction(trans);

        TC_LOG_INFO("playerbot", "{} joined guild '{}' on {}", bot->GetName(), guild->GetName(), master->GetName());
        return true;
    }

    bool LeaveGuild(Player* bot)
    {
        if (!bot || !bot->GetGuildId())
            return false;

        WorldPackets::Guild::GuildLeave leave{WorldPacket(CMSG_GUILD_LEAVE)};
        bot->GetSession()->HandleGuildLeave(leave);
        return true;
    }

    bool SellJunk(Player* bot, std::string& reply)
    {
        if (!bot || !bot->IsAlive() || !bot->IsInWorld())
        {
            reply = "I cannot trade right now";
            return false;
        }

        bool didSomething = false;

        // 1. vendor: dump every gray item (and junk whites) for coin
        if (Creature* vendor = FindNearNpc(bot, UNIT_NPC_FLAG_VENDOR, sBotConfig->SightDistance))
        {
            Creature* interactable = bot->GetNPCIfCanInteractWith(vendor->GetGUID(), UNIT_NPC_FLAG_VENDOR, UNIT_NPC_FLAG_2_NONE);
            if (interactable)
            {
                uint32 sold = 0;
                auto sellGray = [&](Item* item) -> bool
                {
                    ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
                    if (!proto || !proto->GetSellPrice() || item->IsBoundAccountWide())
                        return true;
                    if (proto->GetQuality() > ITEM_QUALITY_NORMAL)
                        return true;
                    if (proto->IsConjuredConsumable())
                        return true;

                    WorldPackets::Item::SellItem packet{WorldPacket(CMSG_SELL_ITEM)};
                    packet.VendorGUID = vendor->GetGUID();
                    packet.ItemGUID = item->GetGUID();
                    packet.Amount = item->GetCount();
                    bot->GetSession()->HandleSellItemOpcode(packet);
                    ++sold;
                    return true;
                };

                for (uint8 i = 0; i < 4; ++i)
                    if (Bag* bag = bot->GetBagByPos(i))
                        for (uint8 slot = 0; slot < bag->GetBagSize(); ++slot)
                            if (Item* item = bag->GetItemByPos(slot))
                                sellGray(item);

                for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                    if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                        sellGray(item);

                if (sold)
                {
                    reply = "sold " + std::to_string(sold) + " junk item(s) to the vendor";
                    didSomething = true;
                }
            }
        }

        // 2. auction house: post unneeded equipment for other players to buy
        if (Creature* auctioneer = FindNearNpc(bot, UNIT_NPC_FLAG_AUCTIONEER, sBotConfig->SightDistance))
        {
            Creature* interactable = bot->GetNPCIfCanInteractWith(auctioneer->GetGUID(), UNIT_NPC_FLAG_AUCTIONEER, UNIT_NPC_FLAG_2_NONE);
            if (!interactable)
            {
                if (!didSomething)
                    reply = "I can see the auctioneer - walk me closer";
            }
            else
            {
                uint32 posted = 0;
                auto postItem = [&](Item* item) -> bool
                {
                    if (posted >= 5)
                        return false;
                    ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
                    if (!proto || proto->GetMaxStackSize() != 1)
                        return true;
                    if (proto->GetQuality() > ITEM_QUALITY_NORMAL || proto->GetClass() == ITEM_CLASS_QUEST)
                        return true;
                    if (!proto->GetSellPrice() || item->IsBoundAccountWide())
                        return true;
                    // keep things we could actually wear
                    if (proto->IsArmor() || proto->IsWeapon())
                        if (bot->CanUseItem(proto, false) == EQUIP_ERR_OK)
                            return true;

                    uint64 const minBid = std::max<uint64>(2000, proto->GetSellPrice() * 4);
                    WorldPackets::AuctionHouse::AuctionSellItem packet{WorldPacket(CMSG_AUCTION_SELL_ITEM)};
                    packet.Auctioneer = auctioneer->GetGUID();
                    packet.MinBid = minBid;
                    packet.BuyoutPrice = minBid * 3;
                    packet.RunTime = MIN_AUCTION_TIME / MINUTE;
                    packet.Items.resize(1);
                    packet.Items[0].Guid = item->GetGUID();
                    packet.Items[0].UseCount = 1;
                    bot->GetSession()->HandleAuctionSellItem(packet);
                    ++posted;
                    return posted < 5;
                };

                for (uint8 i = 0; i < 4 && posted < 5; ++i)
                    if (Bag* bag = bot->GetBagByPos(i))
                        for (uint8 slot = 0; slot < bag->GetBagSize() && posted < 5; ++slot)
                            if (Item* item = bag->GetItemByPos(slot))
                                postItem(item);

                for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END && posted < 5; ++slot)
                    if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                        postItem(item);

                if (posted)
                {
                    reply = "posted " + std::to_string(posted) + " item(s) on the auction house";
                    didSomething = true;
                }
            }
        }

        if (!didSomething)
        {
            if (reply.empty())
                reply = "I need to stand next to a vendor or auctioneer";
            return false;
        }
        return true;
    }

    uint32 TakeMastersQuests(Player* bot, Player* master)
    {
        if (!bot || !master || bot->GetTeam() != master->GetTeam())
            return 0;

        uint32 taken = 0;
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = master->GetQuestSlotQuestId(slot);
            if (!questId)
                continue;
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest || bot->GetQuestStatus(questId) != QUEST_STATUS_NONE)
                continue;
            if (quest->GetQuestMinLevel() > int32(bot->GetLevel()))
                continue;
            if (!bot->CanAddQuest(quest, false))
                continue;

            bot->AddQuest(quest, master);
            ++taken;
        }

        if (taken)
            TC_LOG_INFO("playerbot", "{} took {} quest(s) from {}", bot->GetName(), taken, master->GetName());
        return taken;
    }

    uint32 TurnInCompletedQuests(Player* bot)
    {
        if (!bot || !bot->IsAlive() || !bot->IsInWorld())
            return 0;

        bool anyReady = false;
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE && !anyReady; ++slot)
        {
            uint32 const questId = bot->GetQuestSlotQuestId(slot);
            if (questId && bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE && bot->CanCompleteQuest(questId))
                anyReady = true;
        }
        if (!anyReady)
            return 0;

        Creature* giver = FindQuestGiver(bot);
        if (!giver)
            return 0;

        uint32 turned = 0;
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = bot->GetQuestSlotQuestId(slot);
            if (!questId)
                continue;
            if (bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE || !bot->CanCompleteQuest(questId))
                continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;

            // same order a real client sends: gossip, then the reward choice
            WorldPackets::Quest::QuestGiverHello hello{WorldPacket(CMSG_QUEST_GIVER_HELLO)};
            hello.QuestGiverGUID = giver->GetGUID();
            bot->GetSession()->HandleQuestgiverHelloOpcode(hello);

            WorldPackets::Quest::QuestGiverChooseReward reward{WorldPacket(CMSG_QUEST_GIVER_CHOOSE_REWARD)};
            reward.QuestGiverGUID = giver->GetGUID();
            reward.QuestID = int32(questId);
            if (quest->GetRewChoiceItemsCount() > 0 && quest->RewardChoiceItemId[0])
            {
                reward.Choice.Item.ItemID = quest->RewardChoiceItemId[0];
                reward.Choice.LootItemType = LootItemType::Item;
            }
            bot->GetSession()->HandleQuestgiverChooseRewardOpcode(reward);
            ++turned;
        }

        if (turned)
            TC_LOG_INFO("playerbot", "{} turned in {} quest(s)", bot->GetName(), turned);
        return turned;
    }
}
