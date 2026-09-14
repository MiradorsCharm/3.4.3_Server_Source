/*
 * Playerbot AI - world placement for random bots.
 *
 * Every random bot character is created by the core's own character-creation
 * path, so a freshly created pool starts life in its race's starting zone:
 * five hundred bots means a couple of hundred humans stacked on Northshire
 * Abbey. The level band in the config says nothing about where those bots
 * should be, so they stayed there.
 *
 * This module spreads them over the world, and it does that from data the
 * server already has rather than from a hardcoded coordinate list: the
 * `creature` spawn table. Every spawn is a real, walkable, ground-level
 * position, and every spawned creature knows its own level range - so
 * "a spot that suits a level 37 bot" is just "somewhere a level 37 mob
 * lives". No invented Z coordinates, nothing to maintain when the world
 * database changes.
 *
 * Spots are bucketed by level once (lazily) and then handed out by a simple
 * occupancy heuristic: a 200-yard cell counter, incremented every time a bot
 * is placed, so the next bot is pushed into a different cell instead of
 * joining the queue.
 */

#ifndef PLAYERBOT_BOT_SPAWNS_H
#define PLAYERBOT_BOT_SPAWNS_H

#include "Define.h"
#include "Position.h"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

class Player;

namespace BotSpawns
{
    struct Spot
    {
        uint32 mapId = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float orientation = 0.0f;
        uint8 minLevel = 1;
        uint8 maxLevel = 1;
        uint32 creatureEntry = 0;
    };

    /// Build the candidate table from the creature spawn store. Idempotent.
    void BuildSpots();

    /// Number of usable spots (0 means the world database has no suitable
    /// spawns - the caller then leaves bots where they are).
    size_t GetSpotCount();

    /// A spot whose level band contains 'level', in the emptiest cell we can
    /// find. 'exclude*' with a non-zero minExclusionDistance keeps the result
    /// away from a position (normally the bot's current one).
    bool PickSpot(uint8 level, uint32 excludeMap, float excludeX, float excludeY,
        float minExclusionDistance, Spot& out);

    /// Count a bot as living in the cell that contains this position.
    void NoteOccupancy(uint32 mapId, float x, float y);
    void ClearOccupancy();

    /// Teleport a random bot to a level-appropriate, uncrowded spot. Returns
    /// true when the bot actually moved. A bot that already sits in a fitting
    /// area is left alone.
    bool PlaceRandomBot(Player* bot, float minDistanceFromCurrent);
}

#endif
