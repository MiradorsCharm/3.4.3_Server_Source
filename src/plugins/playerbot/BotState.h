/*
 * Playerbot AI - persistent bot state.
 *
 * A bot is a real character, so its position, level, gear and spells already
 * live in the `characters` database and come back exactly as they were when
 * the core loads the character. What the core does NOT know is that a
 * character was *being a bot*: that it was online, who owned it, and what it
 * had been told to do.
 *
 * This module keeps that in `characters_playerbot`, one row per bot:
 *
 *   guid          character guid (primary key)
 *   master        owning character (0 = unowned/random)
 *   random_bot    part of the random pool
 *   tank_mode     "tank" role was ordered
 *   grind_mode    "grind" was ordered (random bots default to on)
 *   stay          "stay" was ordered
 *   stay_x/y/z    the point a "stay" bot holds
 *   prepared_level  level the bot was last prepared for (spells/gear)
 *   last_seen     unix time of the last state save
 *
 * On startup the roster is read back and the bots are logged in *staggered*
 * (see BotManager's login pacing) at the position the core saved for them,
 * with the roles they had. Nothing is reset and nothing piles up in one tick.
 */

#ifndef PLAYERBOT_BOT_STATE_H
#define PLAYERBOT_BOT_STATE_H

#include "Define.h"
#include "ObjectGuid.h"

#include <string>
#include <vector>

struct BotSavedState
{
    ObjectGuid guid;
    ObjectGuid master;
    bool random = false;
    bool tankMode = false;
    bool grindMode = false;
    bool stay = false;
    float stayX = 0.0f;
    float stayY = 0.0f;
    float stayZ = 0.0f;
    uint8 preparedLevel = 0;      // 0 = never prepared (fresh character)
    uint32 lastSeen = 0;          // unix time
    bool present = false;         // a row existed for this guid
};

namespace BotState
{
    /// Create the table / add missing columns. Safe to call on every start.
    void EnsureSchema();

    /// Every saved bot (both random pool and master-bound), oldest first.
    bool LoadAll(std::vector<BotSavedState>& out);

    /// One saved bot. Returns an empty state (present == false) when unknown.
    BotSavedState Load(ObjectGuid guid);

    /// Upsert one row. Never wipes columns the caller did not fill: the row is
    /// updated field by field.
    void Save(BotSavedState const& state);

    /// Remember only that the bot was prepared for a level (cheap, frequent).
    void SetPreparedLevel(ObjectGuid guid, uint8 level);

    /// Remember only the ownership (used by ".bot add"/".bot remove").
    void SetMaster(ObjectGuid botGuid, ObjectGuid masterGuid);

    void Forget(ObjectGuid guid);

    /// How many rows the table holds (reported by ".bot state").
    uint32 Count();
}

#endif
