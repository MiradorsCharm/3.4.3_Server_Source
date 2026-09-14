/*
 * Playerbot AI - character preparation.
 *
 * Two jobs:
 *  - PrepareBot(): after a bot logs in, make sure the character can actually
 *    fight (learned class spells, a usable weapon, ammo for hunters, sane
 *    level/money). Characters the operator made by hand are only *repaired*,
 *    never re-customized.
 *  - CreateBotCharacter(): a fresh random bot character built through the
 *    same Player::Create path the character screen uses.
 *
 * Gear is chosen from the world database with plain SQL (best usable item of
 * the right type) and every pick is re-verified through Player::CanUseItem /
 * CanEquipItem before it is equipped, so content packs with unusual items
 * cannot break the bots.
 */

#ifndef PLAYERBOT_BOT_FACTORY_H
#define PLAYERBOT_BOT_FACTORY_H

#include "Define.h"

#include <cstdint>
#include <string>

class Player;

namespace BotFactory
{
    /// called once after the bot entered the world
    void PrepareBot(Player* bot);

    /// create a new character for an existing (bot) account; returns the new guid or empty
    bool CreateBotCharacter(uint32 accountId, std::string const& name, uint8 playerClass, uint8 race, uint8 gender);

    /// is this account one of ours? (account name prefix check)
    bool IsBotAccount(uint32 accountId);
}

#endif
