#include "BotSpawns.h"

#include "BotConfig.h"
#include "CreatureData.h"
#include "DB2Stores.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SharedDefines.h"
#include "UnitDefines.h"
#include "Util.h"

#include <algorithm>
#include <cmath>

namespace
{
    // 200 yard cells. World coordinates stay well inside +/- 4096 cells, which
    // leaves room for the map id in the same 64 bit key.
    constexpr float OCCUPANCY_CELL_SIZE = 200.0f;
    constexpr int32 OCCUPANCY_CELL_BIAS = 2048;
    constexpr size_t MAX_SPOTS = 30000;
    constexpr size_t MAX_BUCKET_LEVEL = 128;

    // NPCs that only exist to serve players are not "somewhere a mob lives";
    // they cluster in towns and would recreate the pile-up we are fixing.
    constexpr uint64 SERVICE_NPC_FLAGS =
        uint64(UNIT_NPC_FLAG_GOSSIP | UNIT_NPC_FLAG_QUESTGIVER | UNIT_NPC_FLAG_TRAINER |
               UNIT_NPC_FLAG_TRAINER_CLASS | UNIT_NPC_FLAG_TRAINER_PROFESSION |
               UNIT_NPC_FLAG_VENDOR | UNIT_NPC_FLAG_VENDOR_AMMO | UNIT_NPC_FLAG_VENDOR_FOOD |
               UNIT_NPC_FLAG_VENDOR_POISON | UNIT_NPC_FLAG_VENDOR_REAGENT | UNIT_NPC_FLAG_REPAIR |
               UNIT_NPC_FLAG_FLIGHTMASTER | UNIT_NPC_FLAG_SPIRIT_HEALER |
               UNIT_NPC_FLAG_AREA_SPIRIT_HEALER | UNIT_NPC_FLAG_INNKEEPER | UNIT_NPC_FLAG_BANKER |
               UNIT_NPC_FLAG_PETITIONER | UNIT_NPC_FLAG_TABARDDESIGNER | UNIT_NPC_FLAG_BATTLEMASTER |
               UNIT_NPC_FLAG_AUCTIONEER | UNIT_NPC_FLAG_STABLEMASTER | UNIT_NPC_FLAG_GUILD_BANKER |
               UNIT_NPC_FLAG_MAILBOX);

    bool _built = false;
    std::vector<BotSpawns::Spot> _spots;
    std::array<std::vector<uint32>, MAX_BUCKET_LEVEL + 1> _byLevel;
    std::unordered_map<uint64, uint32> _occupancy;

    uint64 CellKey(uint32 mapId, float x, float y)
    {
        int32 const cx = int32(std::floor(x / OCCUPANCY_CELL_SIZE)) + OCCUPANCY_CELL_BIAS;
        int32 const cy = int32(std::floor(y / OCCUPANCY_CELL_SIZE)) + OCCUPANCY_CELL_BIAS;
        return (uint64(mapId) << 42) | (uint64(uint32(cx) & 0x1FFFFF) << 21) | uint64(uint32(cy) & 0x1FFFFF);
    }

    uint32 OccupancyOf(uint32 mapId, float x, float y)
    {
        auto it = _occupancy.find(CellKey(mapId, x, y));
        return it == _occupancy.end() ? 0 : it->second;
    }

    // Pick the emptiest of a random sample of candidates. Sampling instead of a
    // full scan keeps placement O(1)-ish no matter how big the world database
    // is, and the result is still spread out because occupancy is what drives
    // the choice.
    constexpr size_t SAMPLE_SIZE = 192;
}

void BotSpawns::BuildSpots()
{
    if (_built)
        return;
    _built = true;

    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        (void)spawnId;
        if (_spots.size() >= MAX_SPOTS)
            break;

        MapEntry const* mapEntry = sMapStore.LookupEntry(data.mapId);
        if (!mapEntry || mapEntry->Instanceable())
            continue;

        // phased content is invisible to a bot that does not carry the phase
        if (data.phaseGroup || data.phaseId > 1)
            continue;

        CreatureTemplate const* cInfo = sObjectMgr->GetCreatureTemplate(data.id);
        if (!cInfo)
            continue;

        CreatureDifficulty const* diff = cInfo->GetDifficulty(DIFFICULTY_NONE);
        if (!diff)
            continue;

        if (cInfo->Classification != CreatureClassifications::Normal)
            continue;
        if (cInfo->Civilian)
            continue;
        if (cInfo->type == CREATURE_TYPE_CRITTER || cInfo->type == CREATURE_TYPE_NON_COMBAT_PET)
            continue;
        if (cInfo->npcflag & SERVICE_NPC_FLAGS)
            continue;

        uint8 const minLevel = diff->MinLevel ? diff->MinLevel : 1;
        uint8 const maxLevel = diff->MaxLevel ? diff->MaxLevel : minLevel;
        if (maxLevel < minLevel || minLevel > MAX_BUCKET_LEVEL)
            continue;

        Spot spot;
        spot.mapId = data.mapId;
        spot.x = data.spawnPoint.GetPositionX();
        spot.y = data.spawnPoint.GetPositionY();
        spot.z = data.spawnPoint.GetPositionZ();
        spot.orientation = data.spawnPoint.GetOrientation();
        spot.minLevel = minLevel;
        spot.maxLevel = maxLevel;
        spot.creatureEntry = data.id;

        uint32 const index = uint32(_spots.size());
        _spots.push_back(spot);
        for (uint32 lvl = minLevel; lvl <= uint32(maxLevel) && lvl <= MAX_BUCKET_LEVEL; ++lvl)
            _byLevel[lvl].push_back(index);
    }

    TC_LOG_INFO("playerbot", "Random bot spread: {} world spot(s) collected from the creature spawn table",
        _spots.size());
}

size_t BotSpawns::GetSpotCount()
{
    BuildSpots();
    return _spots.size();
}

bool BotSpawns::PickSpot(uint8 level, uint32 excludeMap, float excludeX, float excludeY,
    float minExclusionDistance, Spot& out)
{
    BuildSpots();
    if (_spots.empty())
        return false;

    uint32 const wanted = std::clamp<uint32>(level, 1, MAX_BUCKET_LEVEL);

    // widen the level window when nothing matches exactly (a level 1 band is
    // often a single starting zone)
    std::vector<uint32> const* candidates = nullptr;
    for (uint32 widen = 0; widen <= 5; ++widen)
    {
        for (int32 sign : { 1, -1 })
        {
            int64 const probe = int64(wanted) + sign * int64(widen);
            if (probe < 1 || probe > int64(MAX_BUCKET_LEVEL))
                continue;
            if (!_byLevel[size_t(probe)].empty())
            {
                candidates = &_byLevel[size_t(probe)];
                break;
            }
        }
        if (candidates)
            break;
    }
    if (!candidates || candidates->empty())
        return false;

    auto pickBest = [&](bool honourExclusion) -> bool
    {
        uint32 bestScore = 0xFFFFFFFF;
        uint32 bestIndex = 0;
        bool found = false;

        size_t const count = candidates->size();
        size_t const tries = std::min(SAMPLE_SIZE, count);
        for (size_t i = 0; i < tries; ++i)
        {
            uint32 const index = (*candidates)[count == 1 ? 0 : urand(0, uint32(count) - 1)];
            Spot const& spot = _spots[index];

            if (honourExclusion && minExclusionDistance > 0.0f && spot.mapId == excludeMap)
            {
                float const dx = spot.x - excludeX;
                float const dy = spot.y - excludeY;
                if (dx * dx + dy * dy < minExclusionDistance * minExclusionDistance)
                    continue;
            }

            uint32 const score = OccupancyOf(spot.mapId, spot.x, spot.y);
            // random tie-break so equal cells do not always resolve the same way
            if (score < bestScore || (score == bestScore && urand(0, 3) == 0))
            {
                bestScore = score;
                bestIndex = index;
                found = true;
            }
        }

        if (!found)
            return false;
        out = _spots[bestIndex];
        return true;
    };

    if (pickBest(true))
        return true;
    return pickBest(false);
}

void BotSpawns::NoteOccupancy(uint32 mapId, float x, float y)
{
    ++_occupancy[CellKey(mapId, x, y)];
}

void BotSpawns::ClearOccupancy()
{
    _occupancy.clear();
}

bool BotSpawns::PlaceRandomBot(Player* bot, float minDistanceFromCurrent)
{
    if (!bot || !bot->IsInWorld())
        return false;
    if (!sBotConfig->RandomBotSpread)
        return false;

    Spot spot;
    if (!PickSpot(bot->GetLevel(), bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
        minDistanceFromCurrent, spot))
        return false;

    // a little jitter so two bots placed on the same spawn do not overlap
    float const x = spot.x + frand(-4.0f, 4.0f);
    float const y = spot.y + frand(-4.0f, 4.0f);

    if (!bot->TeleportTo(spot.mapId, x, y, spot.z, spot.orientation))
        return false;

    NoteOccupancy(spot.mapId, x, y);

    TC_LOG_DEBUG("playerbot", "{} (level {}) relocated to map {} {:.1f},{:.1f} - {} level {} area",
        bot->GetName(), uint32(bot->GetLevel()), spot.mapId, x, y, spot.creatureEntry,
        uint32(spot.minLevel));
    return true;
}
