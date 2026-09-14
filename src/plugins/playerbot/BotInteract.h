/*
 * Playerbot AI - player interactions: trade, duels, guild, auction house.
 *
 * A bot has no client, so every interaction a real player would answer on
 * screen (a trade window, a duel request, a guild invite) would silently
 * time out. This module answers them through the same WorldSession handlers
 * the client's packets end in - but only from people the bot already trusts
 * (its master, a GM, its own account).
 *
 * Mail a bot receives (items, gold) is collected automatically once per
 * minute while the bot stands at a mailbox; COD mails are never paid.
 */

#ifndef PLAYERBOT_BOT_INTERACT_H
#define PLAYERBOT_BOT_INTERACT_H

#include "Define.h"

#include <string>

class Player;

namespace BotInteract
{
    /// per-tick pump (world thread): accept trades and duels from trusted players
    void PumpSession(Player* bot);

    /// join the master's guild ("guild" whisper); false when not possible
    bool JoinMastersGuild(Player* bot, Player* master);

    /// leave our own guild ("guild leave" whisper)
    bool LeaveGuild(Player* bot);

    /// dispose of junk: vendor-sells gray items, posts equipment upgrades at
    /// an auction house when close enough ("sell" whisper); fills reply
    bool SellJunk(Player* bot, std::string& reply);

    /// buy an item by (partial) name from the vendor we are standing at
    /// ("buy <name>" command); fills reply
    bool BuyItemByName(Player* bot, std::string const& wanted, std::string& reply);

    /// take every quest of the master's log we qualify for ("quests" whisper)
    uint32 TakeMastersQuests(Player* bot, Player* master);

    /// hand in every finished quest at a nearby quest giver (automatic)
    uint32 TurnInCompletedQuests(Player* bot);
}

#endif
