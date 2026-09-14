/*
 * Playerbot AI - talent auto-spec.
 *
 * Bots are real characters and earn talent points like players, but no
 * client ever opens their talent pane: every free point would sit unspent
 * forever. SpendPoints() drains all free points through the same
 * Player::LearnTalent the client's request handler ends in - including its
 * prerequisite, rank and point validation - so a bot's spec is always a
 * legal build. Talents are learned tier by tier, tab by tab.
 */

#ifndef PLAYERBOT_BOT_TALENTS_H
#define PLAYERBOT_BOT_TALENTS_H

#include "Define.h"

class Player;

namespace BotTalents
{
    /// spend every free talent point legally; returns the number spent
    uint32 SpendPoints(Player* bot);
}

#endif
