#include "../pchdef.h"
#include "playerbot.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "RandomPlayerbotFactory.h"
#include "../../server/database/Database/DatabaseEnv.h"
#include "PlayerbotAI.h"
#include "AiFactory.h"
#include "Maps/MapManager.h"
#include "PlayerbotCommandServer.h"
#include "GuildTaskMgr.h"
#include <tuple>

namespace
{
    struct BotWorldSpawn
    {
        WorldLocation location;
        uint32 minLevel, maxLevel;
    };

    std::vector<BotWorldSpawn> BuildBotWorldSpawns(uint32 mapId)
    {
        std::vector<BotWorldSpawn> spawns;
        MapEntry const* map = sMapStore.LookupEntry(mapId);
        if (!map || map->Instanceable())
            return spawns;

        // Use the core's registered normal-difficulty spawn grid, not raw SQL
        // rows (which can include rejected entries, other difficulties, or
        // inactive event/pool spawns). Levels come from normalized templates.
        CellObjectGuidsMap const* cells = sObjectMgr->GetMapObjectGuids(mapId, DIFFICULTY_NONE);
        if (!cells)
            return spawns;
        for (auto const& cell : *cells)
            for (ObjectGuid::LowType guid : cell.second.creatures)
            {
                CreatureData const* data = sObjectMgr->GetCreatureData(guid);
                if (!data || data->mapId != mapId || !data->spawnPoint.IsPositionValid() ||
                    data->phaseId || data->phaseGroup || data->terrainSwapMap != -1)
                    continue;
                CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(data->id);
                if (!creature)
                    continue;
                CreatureDifficulty const* difficulty = creature->GetDifficulty(DIFFICULTY_NONE);
                if (!difficulty || !difficulty->MinLevel || difficulty->MaxLevel < difficulty->MinLevel)
                    continue;
                spawns.push_back({ WorldLocation(mapId, data->spawnPoint), difficulty->MinLevel, difficulty->MaxLevel });
            }
        return spawns;
    }

    std::vector<BotWorldSpawn> const& GetBotWorldSpawns(uint32 mapId)
    {
        // Values (not pointers to reloadable template data), once per map/run.
        static std::map<uint32, std::vector<BotWorldSpawn>> cache;
        auto [it, inserted] = cache.try_emplace(mapId);
        if (inserted)
        {
            it->second = BuildBotWorldSpawns(mapId);
            TC_LOG_INFO("playerbot", "Bot world-data cache: map {} has {} usable normal-difficulty spawns", mapId, it->second.size());
        }
        return it->second;
    }

    void AddBotLevelLocations(std::vector<BotWorldSpawn> const& spawns, uint32 level, uint32 delta,
        float sightDistance, std::vector<WorldLocation>& locations)
    {
        float distance = std::isfinite(sightDistance) && sightDistance > 0.0f ? sightDistance : 0.0f;
        float cellSize = std::max(1.0f, distance);
        float distanceSq = distance * distance;
        using Cell = std::tuple<int32, int32, int32>;
        auto cellFor = [cellSize](WorldLocation const& point)
        {
            return Cell(int32(std::floor(point.GetPositionX() / cellSize)), int32(std::floor(point.GetPositionY() / cellSize)),
                int32(std::floor(point.GetPositionZ() / cellSize)));
        };
        std::map<Cell, std::vector<WorldLocation const*>> dangers;
        for (BotWorldSpawn const& spawn : spawns)
            // A higher-level mob is dangerous, not a lower-level one. Avoid
            // overflow for a large configured level delta.
            if (spawn.maxLevel > level && spawn.maxLevel - level > delta)
                dangers[cellFor(spawn.location)].push_back(&spawn.location);

        for (BotWorldSpawn const& spawn : spawns)
        {
            if (spawn.maxLevel > level || level - spawn.minLevel > delta)
                continue;
            auto [cx, cy, cz] = cellFor(spawn.location);
            bool nearDanger = false;
            for (int32 dx = -1; dx <= 1 && !nearDanger; ++dx)
                for (int32 dy = -1; dy <= 1 && !nearDanger; ++dy)
                    for (int32 dz = -1; dz <= 1 && !nearDanger; ++dz)
                    {
                        auto it = dangers.find(Cell(cx + dx, cy + dy, cz + dz));
                        if (it == dangers.end())
                            continue;
                        for (WorldLocation const* danger : it->second)
                        {
                            float x = danger->GetPositionX() - spawn.location.GetPositionX();
                            float y = danger->GetPositionY() - spawn.location.GetPositionY();
                            float z = danger->GetPositionZ() - spawn.location.GetPositionZ();
                            if (x * x + y * y + z * z < distanceSq)
                            {
                                nearDanger = true;
                                break;
                            }
                        }
                    }
            if (!nearDanger)
                locations.push_back(spawn.location);
        }
    }
}

RandomPlayerbotMgr::RandomPlayerbotMgr() : PlayerbotHolder(), processTicks(0)
{
}

RandomPlayerbotMgr::~RandomPlayerbotMgr()
{
    // Do this while derived members are still alive, before the base destructor.
    LogoutAllBots();
}

void RandomPlayerbotMgr::UpdateAIInternal(uint32 elapsed)
{
    SetNextCheckDelay(sPlayerbotAIConfig.randomBotUpdateInterval * 1000);

    // answer remote command-server requests queued by its worker threads -
    // those threads must never touch world objects themselves
    sPlayerbotCommandServer.ProcessPending();

    if (!sPlayerbotAIConfig.randomBotAutologin || !sPlayerbotAIConfig.enabled)
        return;

    TC_LOG_DEBUG("playerbot",  "Processing random bots...");

    uint32 maxAllowedBotCount = GetEventValue(0, "bot_count");
    if (!maxAllowedBotCount)
    {
        maxAllowedBotCount = urand(sPlayerbotAIConfig.minRandomBots, sPlayerbotAIConfig.maxRandomBots);
        SetEventValue(0, "bot_count", maxAllowedBotCount,
                urand(sPlayerbotAIConfig.randomBotCountChangeMinInterval, sPlayerbotAIConfig.randomBotCountChangeMaxInterval));
    }

    list<uint32> bots = GetBots();
    uint32 botCount = bots.size();
    uint32 randomBotsPerInterval = urand(sPlayerbotAIConfig.minRandomBotsPerInterval, sPlayerbotAIConfig.maxRandomBotsPerInterval);
    if (sPlayerbotAIConfig.randomBotLoginAtStartup && processTicks < 10)
    {
        // spread the startup population push over the first ~10 ticks at 10x
        // the configured interval rate (bounded): every ProcessBot loads a
        // character on the world thread, and logging in the whole population
        // in a single tick stalls long enough for the FreezeDetector to kill
        // the server when RandomBotLoginAtStartup is on
        randomBotsPerInterval = std::min(randomBotsPerInterval, 10u) * 10;
    }
    // processTicks was initialised but never incremented, making the startup
    // branch above fire on EVERY tick - every tick processed the whole bot list
    if (processTicks < 1000)
        ++processTicks;

    // resolve the free bots for this tick ONCE: AddRandomBot used to rescan the
    // characters of every random account per bot added, i.e. accountCount x
    // addedBots character queries in a single world tick (a FreezeDetector trip
    // at startup with a large MaxRandomBots)
    vector<uint32> freeAllianceBots, freeHordeBots;
    GetAllFreeBots(freeAllianceBots, freeHordeBots);

    // Self-heal: no registered bots plus no free characters means zero usable
    // bot characters exist at all (startup creation failed, was skipped, or
    // the database came from elsewhere without them) - retry creation instead
    // of staying empty forever. Throttled: CreateRandomBots is idempotent but
    // does synchronous DB/creation work on the world thread. Skipped while the
    // delete flag is on so a wipe is not immediately recreated every 10 min.
    if (maxAllowedBotCount > 0 && bots.empty() && freeAllianceBots.empty() && freeHordeBots.empty() &&
        !sPlayerbotAIConfig.deleteRandomBotAccounts)
    {
        static time_t lastCreateRetry = 0;
        time_t now = time(nullptr);
        if (now - lastCreateRetry >= 600)
        {
            lastCreateRetry = now;
            TC_LOG_INFO("playerbot", "No random bot characters found - retrying bot creation...");
            RandomPlayerbotFactory::CreateRandomBots();
            freeAllianceBots.clear();
            freeHordeBots.clear();
            GetAllFreeBots(freeAllianceBots, freeHordeBots);
        }
    }

    int addsThisTick = 0;
    while (botCount++ < maxAllowedBotCount && addsThisTick < 200)
    {
        bool alliance = botCount % 2;
        uint32 bot = AddRandomBot(alliance ? freeAllianceBots : freeHordeBots);
        if (bot)
        {
            bots.push_back(bot);
            ++addsThisTick;
        }
        else break;
    }

    uint32 botProcessed = 0;
    for (list<uint32>::iterator i = bots.begin(); i != bots.end(); ++i)
    {
        if (botProcessed >= randomBotsPerInterval)
            break;

        uint32 bot = *i;
        if (ProcessBot(bot))
            botProcessed++;
    }

    TC_LOG_DEBUG("playerbot",  "{} bots processed. Next check in {} seconds",
            botProcessed, sPlayerbotAIConfig.randomBotUpdateInterval);

    // The per-tick population dump used to sit here: twenty INFO lines every minute
    // on top of the per-bot lines. The breakdown is operator-facing output, so it
    // keeps its INFO level and is printed when it is asked for (.rndbot stats).
}

uint32 RandomPlayerbotMgr::AddRandomBot(vector<uint32>& bots)
{
    if (bots.empty())
        return 0;

    int index = urand(0, bots.size() - 1);
    uint32 bot = bots[index];
    // the chosen bot is no longer free - do not offer it again this tick
    bots.erase(bots.begin() + index);
    SetEventValue(bot, "add", 1, urand(sPlayerbotAIConfig.minRandomBotInWorldTime, sPlayerbotAIConfig.maxRandomBotInWorldTime));
    uint32 randomTime = 30 + urand(sPlayerbotAIConfig.randomBotUpdateInterval, sPlayerbotAIConfig.randomBotUpdateInterval * 3);
    ScheduleRandomize(bot, randomTime);
    TC_LOG_DEBUG("playerbot",  "Random bot {} added", bot);
    return bot;
}

void RandomPlayerbotMgr::ScheduleRandomize(uint32 bot, uint32 time)
{
    SetEventValue(bot, "randomize", 1, time);
    SetEventValue(bot, "logout", 1, time + 30 + urand(sPlayerbotAIConfig.randomBotUpdateInterval, sPlayerbotAIConfig.randomBotUpdateInterval * 3));
}

void RandomPlayerbotMgr::ScheduleTeleport(uint32 bot)
{
    SetEventValue(bot, "teleport", 1, 60 + urand(sPlayerbotAIConfig.randomBotUpdateInterval, sPlayerbotAIConfig.randomBotUpdateInterval * 3));
}

bool RandomPlayerbotMgr::ProcessBot(uint32 bot)
{
    uint32 isValid = GetEventValue(bot, "add");
    if (!isValid)
    {
		Player* player = GetPlayerBot(ObjectGuid::Create<HighGuid::Player>(bot));
		if (!player || !player->GetGroup())
		{
			TC_LOG_DEBUG("playerbot",  "Bot {} expired", bot);
			SetEventValue(bot, "add", 0, 0);
		}
        return true;
    }

    if (!GetPlayerBot(ObjectGuid::Create<HighGuid::Player>(bot)))
    {
        AddPlayerBot(ObjectGuid::Create<HighGuid::Player>(bot), 0);
        if (!GetEventValue(bot, "online"))
        {
            SetEventValue(bot, "online", 1, sPlayerbotAIConfig.minRandomBotInWorldTime);
        }
        return true;
    }

    Player* player = GetPlayerBot(ObjectGuid::Create<HighGuid::Player>(bot));
    if (!player || !player->IsInWorld() || player->IsBeingTeleported())
        return false;

    PlayerbotAI* ai = player->GetPlayerbotAI();
    if (!ai)
        return false;

    if (player->GetGroup())
    {
        TC_LOG_DEBUG("playerbot",  "Skipping bot {} as it is in group", bot);
        return false;
    }

    if (player->isDead())
    {
        if (!GetEventValue(bot, "dead"))
        {
            TC_LOG_DEBUG("playerbot",  "Setting dead flag for bot {}", bot);
            uint32 randomTime = urand(sPlayerbotAIConfig.minRandomBotReviveTime, sPlayerbotAIConfig.maxRandomBotReviveTime);
            SetEventValue(bot, "dead", 1, randomTime);
            // guard the -60 offset: a revive time below 60 would wrap the uint32
            // validIn around 4 billion seconds and the bot would never revive
            SetEventValue(bot, "revive", 1, randomTime > 60 ? randomTime - 60 : 1);
            return false;
        }

        if (!GetEventValue(bot, "revive"))
        {
            TC_LOG_DEBUG("playerbot",  "Reviving dead bot {}", bot);
            SetEventValue(bot, "dead", 0, 0);
            SetEventValue(bot, "revive", 0, 0);
            RandomTeleport(player, player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
            return true;
        }

        return false;
    }

    if (player->GetGuild() && player->GetGuild()->GetLeaderGUID() == player->GetGUID())
    {
        for (ObjectGuid guid : players)
            if (Player* realPlayer = ObjectAccessor::FindPlayer(guid))
                if (realPlayer->GetSession() && !realPlayer->GetSession()->IsBotSession())
                    sGuildTaskMgr.Update(realPlayer, player);
    }

    uint32 randomize = GetEventValue(bot, "randomize");
    if (!randomize)
    {
        TC_LOG_DEBUG("playerbot",  "Randomizing bot {}", bot);
        Randomize(player);
        uint32 randomTime = urand(sPlayerbotAIConfig.minRandomBotRandomizeTime, sPlayerbotAIConfig.maxRandomBotRandomizeTime);
        ScheduleRandomize(bot, randomTime);
        return true;
    }

    uint32 logout = GetEventValue(bot, "logout");
    if (!logout)
    {
        TC_LOG_DEBUG("playerbot",  "Logging out bot {}", bot);
        LogoutPlayerBot(ObjectGuid::Create<HighGuid::Player>(bot));
        SetEventValue(bot, "logout", 1, sPlayerbotAIConfig.maxRandomBotInWorldTime);
        return true;
    }

    uint32 teleport = GetEventValue(bot, "teleport");
    if (!teleport)
    {
        TC_LOG_DEBUG("playerbot",  "Random teleporting bot {}", bot);
        RandomTeleportForLevel(ai->GetBot());
        SetEventValue(bot, "teleport", 1, sPlayerbotAIConfig.maxRandomBotInWorldTime);
        return true;
    }

    return false;
}

void RandomPlayerbotMgr::RandomTeleport(Player* bot, vector<WorldLocation> &locs)
{
    if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported())
        return;

    if (locs.empty())
    {
        TC_LOG_DEBUG("playerbot",  "Cannot teleport bot {} - no locations available", bot->GetName().c_str());
        return;
    }

    for (int attemtps = 0; attemtps < 10; ++attemtps)
    {
        int index = urand(0, locs.size() - 1);
        WorldLocation loc = locs[index];
        float distance = sPlayerbotAIConfig.grindDistance;
        if (!std::isfinite(distance) || distance < 0.0f)
            distance = 0.0f;
        float x = loc.m_positionX + frand(0.0f, distance) - distance / 2;
        float y = loc.m_positionY + frand(0.0f, distance) - distance / 2;
        float z = loc.m_positionZ;

        MapEntry const* entry = sMapStore.LookupEntry(loc.GetMapId());
        if (!entry || entry->Instanceable() || !Trinity::IsValidMapCoord(x, y, z))
            continue;

        Map* map = sMapMgr->CreateMap(loc.GetMapId(), bot);
        if (!map)
            continue;

        if (map->IsInWater(bot->GetPhaseShift(), x, y, z))
            continue;

        uint32 areaId = map->GetAreaId(bot->GetPhaseShift(), x, y, z);
        if (!areaId)
            continue;

		AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId);
        if (!area)
            continue;

        float ground = map->GetHeight(bot->GetPhaseShift(), x, y, z + 0.5f);
        if (!Trinity::IsValidMapCoord(ground) || ground <= INVALID_HEIGHT)
            continue;

        z = 0.05f + ground;
        // Dest tiles are often not loaded (and ocean/empty grids have no .mmtile).
        // Requiring a navmesh polygon at the grind offset therefore rejects valid
        // creature-spawn destinations and logs a false mmap error even when
        // bots already walk with MotionMaster pathfinding after spawn.
        TC_LOG_DEBUG("playerbot",  "Random teleporting bot {} to {} {},{},{} (1/{} locations)",
                bot->GetName().c_str(), area->AreaName[LOCALE_enUS], x, y, z, locs.size());

        bot->GetMotionMaster()->Clear();
        bot->TeleportTo(loc.GetMapId(), x, y, z, 0);
        return;
    }

    TC_LOG_DEBUG("playerbot",  "Cannot teleport bot {} - no candidate passed terrain/area checks", bot->GetName().c_str());
}

void RandomPlayerbotMgr::RandomTeleportForLevel(Player* bot)
{
    if (!bot || !bot->GetPlayerbotAI() || !bot->IsInWorld() || bot->IsBeingTeleported())
        return;

    if (sPlayerbotAIConfig.randomBotMaps.empty())
    {
        TC_LOG_ERROR("playerbot", "Cannot teleport bot {} - no valid AiPlayerbot.RandomBotMaps configured", bot->GetName());
        return;
    }

    TC_LOG_DEBUG("playerbot",  "Preparing location to random teleporting bot {} for level {}", bot->GetName().c_str(), bot->GetLevel());

    // levels are tried exactly once per server run: an empty candidate list
    // must not re-run the full creature scan on every teleport of a bot with
    // that level
    static set<uint8> teleLevelsTried;

    if (teleLevelsTried.insert(bot->GetLevel()).second)
        for (uint32 mapId : sPlayerbotAIConfig.randomBotMaps)
            AddBotLevelLocations(GetBotWorldSpawns(mapId), bot->GetLevel(), sPlayerbotAIConfig.randomBotTeleLevel,
                sPlayerbotAIConfig.sightDistance, locsPerLevelCache[bot->GetLevel()]);

    RandomTeleport(bot, locsPerLevelCache[bot->GetLevel()]);
}

void RandomPlayerbotMgr::RandomTeleport(Player* bot, uint16 mapId, float teleX, float teleY, float teleZ)
{
    TC_LOG_DEBUG("playerbot",  "Preparing location to random teleporting bot {}", bot->GetName().c_str());

    vector<WorldLocation> locs;
    float radius = sPlayerbotAIConfig.randomBotTeleportDistance / 2.0f;
    for (BotWorldSpawn const& spawn : GetBotWorldSpawns(mapId))
        if (std::abs(spawn.location.GetPositionX() - teleX) < radius && std::abs(spawn.location.GetPositionY() - teleY) < radius)
            locs.push_back(spawn.location);

    // End combat/resurrect while the bot is still on its current map.
    Refresh(bot);
    RandomTeleport(bot, locs);
}

void RandomPlayerbotMgr::Randomize(Player* bot)
{
    if (!bot || !bot->GetPlayerbotAI() || !bot->IsInWorld() || bot->IsBeingTeleported())
        return;

    if (bot->GetLevel() == 1)
        RandomizeFirst(bot);
    else
        IncreaseLevel(bot);
}

void RandomPlayerbotMgr::IncreaseLevel(Player* bot)
{
    if (!bot || !bot->GetPlayerbotAI() || !bot->IsInWorld() || bot->IsBeingTeleported())
        return;

    uint32 maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
    uint32 level = min((uint32)(bot->GetLevel() + 1), maxLevel);
    PlayerbotFactory factory(bot, level);
    if (bot->GetGuildId())
        factory.Refresh();
    else
        factory.Randomize();
    RandomTeleportForLevel(bot);
}

void RandomPlayerbotMgr::RandomizeFirst(Player* bot)
{
    if (!bot || !bot->GetPlayerbotAI() || !bot->IsInWorld() || bot->IsBeingTeleported())
        return;

    uint32 maxLevel = std::clamp<uint32>(sPlayerbotAIConfig.randomBotMaxLevel, 1,
        sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));

    if (sPlayerbotAIConfig.randomBotMaps.empty())
    {
        TC_LOG_ERROR("playerbot", "Cannot randomize bot {} - AiPlayerbot.RandomBotMaps is empty", bot->GetName());
        return;
    }

    for (int attempt = 0; attempt < 10; ++attempt)
    {
        int index = urand(0, sPlayerbotAIConfig.randomBotMaps.size() - 1);
        uint16 mapId = sPlayerbotAIConfig.randomBotMaps[index];

        vector<GameTele const*> locs;
        GameTeleContainer const & teleMap = sObjectMgr->GetGameTeleMap();
        for(GameTeleContainer::const_iterator itr = teleMap.begin(); itr != teleMap.end(); ++itr)
        {
            GameTele const* tele = &itr->second;
            if (tele->mapId == mapId)
                locs.push_back(tele);
        }

        // no teleport locations defined for this map (empty/stripped game_tele):
        // indexing locs would underflow urand() and dereference
        if (locs.empty())
        {
            TC_LOG_DEBUG("playerbot", "No game_tele locations for map {}, skipping for random teleport of bot {}",
                    mapId, bot->GetName());
            continue;
        }

        index = urand(0, locs.size() - 1);
        GameTele const* tele = locs[index];
        uint32 level = GetZoneLevel(tele->mapId, tele->position_x, tele->position_y, tele->position_z);
        if (level > maxLevel + 5)
            continue;

        level = min(level, maxLevel);
        if (!level) level = 1;

        if (urand(0, 100) < 100 * sPlayerbotAIConfig.randomBotMaxLevelChance)
            level = maxLevel;

        if (level < sPlayerbotAIConfig.randomBotMinLevel)
            continue;

        PlayerbotFactory factory(bot, level);
        factory.CleanRandomize();
        RandomTeleport(bot, tele->mapId, tele->position_x, tele->position_y, tele->position_z);
        break;
    }
}

uint32 RandomPlayerbotMgr::GetZoneLevel(uint16 mapId, float teleX, float teleY, float /*teleZ*/)
{
    uint32 maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
    if (!Trinity::IsValidMapCoord(teleX, teleY))
        return 1;

    uint64 minTotal = 0, maxTotal = 0, count = 0;
    float radius = sPlayerbotAIConfig.randomBotTeleportDistance / 2.0f;
    for (BotWorldSpawn const& spawn : GetBotWorldSpawns(mapId))
    {
        if (spawn.minLevel <= 1 || std::abs(spawn.location.GetPositionX() - teleX) >= radius ||
            std::abs(spawn.location.GetPositionY() - teleY) >= radius)
            continue;
        minTotal += spawn.minLevel;
        maxTotal += spawn.maxLevel;
        ++count;
    }
    if (!count)
        return urand(1, maxLevel);

    uint32 minLevel = std::clamp<uint32>(uint32(minTotal / count), 1, maxLevel);
    uint32 maxZoneLevel = std::clamp<uint32>(uint32(maxTotal / count), minLevel, maxLevel);
    return urand(minLevel, maxZoneLevel);
}

void RandomPlayerbotMgr::Refresh(Player* bot)
{
    if (!bot || !bot->GetPlayerbotAI())
        return;

    TC_LOG_DEBUG("playerbot",  "Refreshing bot {}", bot->GetName().c_str());
    if (bot->isDead())
    {
        bot->ResurrectPlayer(1.0f);
        bot->SpawnCorpseBones();
        bot->SaveToDB();
        bot->GetPlayerbotAI()->ResetStrategies();
    }

    bot->GetPlayerbotAI()->Reset();

    // CombatStop on an opponent deletes entries from our threatened-by-me
    // container, invalidating an iterator over it. Let the core end this bot's
    // combat instead; it removes reciprocal references without stopping the
    // opponent's combat with unrelated real players.
    bot->CombatStop(true);
    bot->GetThreatManager().RemoveMeFromThreatLists();

    bot->DurabilityRepairAll(false, 1.0f, false);
    bot->SetFullHealth();
    bot->SetPvP(true);

    if (bot->GetMaxPower(POWER_MANA) > 0)
        bot->SetPower(POWER_MANA, bot->GetMaxPower(POWER_MANA));

    if (bot->GetMaxPower(POWER_ENERGY) > 0)
        bot->SetPower(POWER_ENERGY, bot->GetMaxPower(POWER_ENERGY));
}


bool RandomPlayerbotMgr::IsRandomBot(Player* bot)
{
    return IsRandomBot(bot->GetGUID().GetCounter());
}

bool RandomPlayerbotMgr::IsRandomBot(uint32 bot)
{
    return GetEventValue(bot, "add");
}

list<uint32> RandomPlayerbotMgr::GetBots()
{
    list<uint32> bots;

    QueryResult results = CharacterDatabase.Query(
            "select bot from ai_playerbot_random_bots where owner = 0 and event = 'add'");

    if (results)
    {
        do
        {
            Field* fields = results->Fetch();
            uint32 bot = fields[0].GetUInt32();
            bots.push_back(bot);
        } while (results->NextRow());
    }

    return bots;
}

void RandomPlayerbotMgr::GetAllFreeBots(vector<uint32>& freeAllianceBots, vector<uint32>& freeHordeBots)
{
    set<uint32> bots;

    QueryResult results = CharacterDatabase.PQuery(
            "select `bot` from ai_playerbot_random_bots where event = 'add'");

    if (results)
    {
        do
        {
            Field* fields = results->Fetch();
            uint32 bot = fields[0].GetUInt32();
            bots.insert(bot);
        } while (results->NextRow());
    }

    for (list<uint32>::iterator i = sPlayerbotAIConfig.randomBotAccounts.begin(); i != sPlayerbotAIConfig.randomBotAccounts.end(); i++)
    {
        uint32 accountId = *i;
        if (!sAccountMgr->GetCharactersCount(accountId))
            continue;

        QueryResult result = CharacterDatabase.PQuery("SELECT guid, race FROM characters WHERE account = '{}'", accountId);
        if (!result)
            continue;

        do
        {
            Field* fields = result->Fetch();
            uint32 guid = fields[0].GetUInt32();
            uint8 race = fields[1].GetUInt8();
            if (bots.find(guid) == bots.end())
            {
                if (IsAlliance(race))
                    freeAllianceBots.push_back(guid);
                else
                    freeHordeBots.push_back(guid);
            }
        } while (result->NextRow());
    }
}

vector<uint32> RandomPlayerbotMgr::GetFreeBots(bool alliance)
{
    vector<uint32> freeAllianceBots, freeHordeBots;
    GetAllFreeBots(freeAllianceBots, freeHordeBots);
    return alliance ? freeAllianceBots : freeHordeBots;
}

uint32 RandomPlayerbotMgr::GetEventValue(uint32 bot, string event)
{
    uint32 value = 0;

    QueryResult results = CharacterDatabase.PQuery(
            "select `value`, `time`, validIn from ai_playerbot_random_bots where owner = 0 and bot = '{}' and event = '{}'",
            bot, event.c_str());

    if (results)
    {
        Field* fields = results->Fetch();
        value = fields[0].GetUInt32();
        uint32 lastChangeTime = fields[1].GetUInt32();
        uint32 validIn = fields[2].GetUInt32();
        if ((time(0) - lastChangeTime) >= validIn)
            value = 0;
    }

    return value;
}

uint32 RandomPlayerbotMgr::SetEventValue(uint32 bot, string event, uint32 value, uint32 validIn)
{
    CharacterDatabase.PExecute("delete from ai_playerbot_random_bots where owner = 0 and bot = '{}' and event = '{}'",
            bot, event.c_str());
    if (value)
    {
        CharacterDatabase.PExecute(
                "insert into ai_playerbot_random_bots (owner, bot, `time`, validIn, event, `value`) values ('{}', '{}', '{}', '{}', '{}', '{}')",
                0, bot, (uint32)time(0), validIn, event.c_str(), value);
    }

    return value;
}

bool RandomPlayerbotMgr::HandlePlayerbotConsoleCommand(ChatHandler* handler, char const* args)
{
    if (!sPlayerbotAIConfig.enabled)
    {
        TC_LOG_ERROR("playerbot",  "Playerbot system is currently disabled!");
        return false;
    }

    if (!args || !*args)
    {
        TC_LOG_ERROR("playerbot",  "Usage: rndbot stats/update/reset/init/refresh/add/remove (init also creates any missing bot accounts/characters)");
        return false;
    }

    string cmd = args;

    if (cmd == "reset")
    {
        CharacterDatabase.PExecute("delete from ai_playerbot_random_bots");
        TC_LOG_INFO("playerbot",  "Random bots were reset for all players. Please restart the Server.");
        return true;
    }
    else if (cmd == "stats")
    {
        sRandomPlayerbotMgr.PrintStats();
        return true;
    }
    else if (cmd == "update")
    {
        sRandomPlayerbotMgr.UpdateAIInternal(0);
        return true;
    }
    else if (cmd == "init" || cmd == "refresh" || cmd == "teleport")
    {
        if (cmd == "init")
        {
            // (Re)create any missing bot accounts/characters first: with no
            // characters at all there is nothing online to randomize, so plain
            // "init" used to silently do nothing on a fresh/empty database.
            // New characters log in via the normal autologin ticks below.
            TC_LOG_INFO("playerbot", "Ensuring random bot accounts/characters exist...");
            RandomPlayerbotFactory::CreateRandomBots();
        }
		TC_LOG_INFO("playerbot",  "Randomizing bots for {} accounts", sPlayerbotAIConfig.randomBotAccounts.size());
        list<uint32> botIds;
        for (list<uint32>::iterator i = sPlayerbotAIConfig.randomBotAccounts.begin(); i != sPlayerbotAIConfig.randomBotAccounts.end(); ++i)
        {
            uint32 account = *i;
            if (QueryResult results = CharacterDatabase.PQuery("SELECT guid FROM characters where account = '{}'", account))
            {
                do
                {
                    Field* fields = results->Fetch();

                    uint32 botId = fields[0].GetUInt32();
                    ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(botId);
                    Player* bot = ObjectAccessor::FindPlayer(guid);
                    if (!bot)
                        continue;

                    botIds.push_back(botId);
                } while (results->NextRow());
            }
        }

        int processed = 0;
        for (list<uint32>::iterator i = botIds.begin(); i != botIds.end(); ++i)
        {
            ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(*i);
            Player* bot = ObjectAccessor::FindPlayer(guid);
            if (!bot || !bot->GetPlayerbotAI() || !bot->IsInWorld() || bot->IsBeingTeleported())
                continue;

            TC_LOG_INFO("playerbot",  "[{}/{}] Processing command '{}' for bot '{}'",
                    processed++, botIds.size(), cmd.c_str(), bot->GetName().c_str());

            if (cmd == "init")
            {
                sRandomPlayerbotMgr.RandomizeFirst(bot);
            }
            else if (cmd == "teleport")
            {
                sRandomPlayerbotMgr.RandomTeleportForLevel(bot);
            }
            else
            {
                // Refresh at the current level without temporarily setting a
                // level-1 bot to level 0 (or leaving its stats out of sync).
                PlayerbotFactory factory(bot, bot->GetLevel());
                factory.Refresh();
                sRandomPlayerbotMgr.RandomTeleportForLevel(bot);
            }
            uint32 randomTime = urand(sPlayerbotAIConfig.minRandomBotRandomizeTime, sPlayerbotAIConfig.maxRandomBotRandomizeTime);
            CharacterDatabase.PExecute("update ai_playerbot_random_bots set validIn = '{}' where event = 'randomize' and bot = '{}'",
                    randomTime, bot->GetGUID().GetCounter());
            CharacterDatabase.PExecute("update ai_playerbot_random_bots set validIn = '{}' where event = 'logout' and bot = '{}'",
                    sPlayerbotAIConfig.maxRandomBotInWorldTime, bot->GetGUID().GetCounter());
        }
        return true;
    }
    else
    {
        list<string> messages = sRandomPlayerbotMgr.HandlePlayerbotCommand(args, NULL);
        for (list<string>::iterator i = messages.begin(); i != messages.end(); ++i)
        {
            TC_LOG_INFO("playerbot",  "{}", i->c_str());
        }
        return true;
    }

    return false;
}

void RandomPlayerbotMgr::HandleCommand(uint32 type, const string& text, Player& fromPlayer)
{
    for (PlayerBotMap::const_iterator it = GetPlayerBotsBegin(); it != GetPlayerBotsEnd(); ++it)
    {
        Player* const bot = it->second;
        PlayerbotAI* ai = bot->GetPlayerbotAI();
        if (!ai)
            continue;

        ai->HandleCommand(type, text, fromPlayer);
    }
}

void RandomPlayerbotMgr::OnPlayerLogout(Player* player)
{
    if (!player)
        return;
    players.erase(player->GetGUID());
    for (PlayerBotMap::const_iterator it = GetPlayerBotsBegin(); it != GetPlayerBotsEnd(); ++it)
    {
        Player* const bot = it->second;
        PlayerbotAI* ai = bot->GetPlayerbotAI();
        if (!ai)
            continue;

        // session logouts are processed before the next map/player update, but a
        // bot tick after that would still drain chat commands that captured this
        // player as their owner (whisper-then-quit); drop them while he is alive
        ai->DropCommandsFrom(player);

        if (player == ai->GetMaster())
        {
            ai->SetMaster(NULL);
            ai->ResetStrategies();
        }
    }

    // A GUID registry cannot retain a dangling player pointer.
}

void RandomPlayerbotMgr::OnPlayerLogin(Player* player)
{
    if (!player || !player->GetSession() || player->GetSession()->IsBotSession() || player->GetPlayerbotAI())
        return;
    players.insert(player->GetGUID()); // repeated login notifications are idempotent
    for (PlayerBotMap::const_iterator it = GetPlayerBotsBegin(); it != GetPlayerBotsEnd(); ++it)
    {
        Player* const bot = it->second;
        if (player == bot || player->GetPlayerbotAI())
            continue;

        // the AI is attached before a bot enters the map, but guard anyway: a
        // bot that is mid logout/teardown must not be dereferenced here
        PlayerbotAI* ai = bot->GetPlayerbotAI();
        if (!ai)
            continue;

        Group* group = bot->GetGroup();
        if (!group)
            continue;

        for (GroupReference *gref = group->GetFirstMember(); gref; gref = gref->next())
        {
            // offline group members (players that logged out earlier) have a
            // null source; they can never be the player that just logged in
            Player* member = gref->GetSource();
            if (!member)
                continue;

            if (member == player && (!ai->GetMaster() || ai->GetMaster()->GetPlayerbotAI()))
            {
                ai->SetMaster(player);
                ai->ResetStrategies();
                ai->TellMaster("Hello");
                break;
            }
        }
    }
}

Player* RandomPlayerbotMgr::GetRandomPlayer()
{
    std::vector<Player*> candidates;
    for (ObjectGuid guid : players)
        if (Player* player = ObjectAccessor::FindPlayer(guid))
            if (player->GetSession() && !player->GetSession()->IsBotSession())
                candidates.push_back(player);
    return candidates.empty() ? nullptr : candidates[urand(0, candidates.size() - 1)];
}

void RandomPlayerbotMgr::UpdatePlayerbotSessions(uint32 elapsed)
{
    // Login/logout and cross-map session callbacks must run with World, never
    // from a master's Player::Update on a map worker.
    std::vector<ObjectGuid> owners(players.begin(), players.end());
    for (ObjectGuid guid : owners)
    {
        Player* player = ObjectAccessor::FindConnectedPlayer(guid);
        if (!player || !player->GetSession() || player->GetSession()->IsBotSession())
        {
            players.erase(guid);
            continue;
        }
        if (player->GetSession()->PlayerDisconnected())
            continue;
        if (PlayerbotMgr* mgr = player->GetPlayerbotMgr())
        {
            mgr->UpdateAI(elapsed);
            mgr->UpdateSessions(elapsed);
        }
    }
}

void RandomPlayerbotMgr::ShutdownPlayerbotSessions()
{
    std::vector<ObjectGuid> owners(players.begin(), players.end());
    for (ObjectGuid guid : owners)
        if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
            if (PlayerbotMgr* mgr = player->GetPlayerbotMgr())
            {
                player->SetPlayerbotMgr(nullptr);
                delete mgr;
            }
    players.clear();
}

void RandomPlayerbotMgr::PrintStats()
{
    TC_LOG_INFO("playerbot",  "{} Random Bots online", playerBots.size());

    map<uint32, int> alliance, horde;
    for (uint32 i = 0; i < 10; ++i)
    {
        alliance[i] = 0;
        horde[i] = 0;
    }

    map<uint8, int> perRace, perClass;
    for (uint8 race = RACE_HUMAN; race < MAX_RACES; ++race)
    {
        perRace[race] = 0;
    }
    for (uint8 cls = CLASS_WARRIOR; cls < MAX_CLASSES; ++cls)
    {
        perClass[cls] = 0;
    }

    int dps = 0, heal = 0, tank = 0;
    for (PlayerBotMap::iterator i = playerBots.begin(); i != playerBots.end(); ++i)
    {
        Player* bot = i->second;
        if (IsAlliance(bot->GetRace()))
            alliance[bot->GetLevel() / 10]++;
        else
            horde[bot->GetLevel() / 10]++;

        perRace[bot->GetRace()]++;
        perClass[bot->GetClass()]++;

        int spec = AiFactory::GetPlayerSpecTab(bot);
        switch (bot->GetClass())
        {
        case CLASS_DRUID:
            if (spec == 2)
                heal++;
            else
                dps++;
            break;
        case CLASS_PALADIN:
            if (spec == 1)
                tank++;
            else if (spec == 0)
                heal++;
            else
                dps++;
            break;
        case CLASS_PRIEST:
            if (spec != 2)
                heal++;
            else
                dps++;
            break;
        case CLASS_SHAMAN:
            if (spec == 2)
                heal++;
            else
                dps++;
            break;
        case CLASS_WARRIOR:
            if (spec == 2)
                tank++;
            else
                dps++;
            break;
        default:
            dps++;
            break;
        }
    }

    TC_LOG_INFO("playerbot",  "Per level:");
    uint32 maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
    for (uint32 i = 0; i < 10; ++i)
    {
        if (!alliance[i] && !horde[i])
            continue;

        uint32 from = i*10;
        uint32 to = min(from + 9, maxLevel);
        if (!from) from = 1;
        TC_LOG_INFO("playerbot",  "    {}..{}: {} alliance, {} horde", from, to, alliance[i], horde[i]);
    }
    TC_LOG_INFO("playerbot",  "Per race:");
    for (uint8 race = RACE_HUMAN; race < MAX_RACES; ++race)
    {
        if (perRace[race])
            TC_LOG_INFO("playerbot",  "    {}: {}", ChatHelper::formatRace(race).c_str(), perRace[race]);
    }
    TC_LOG_INFO("playerbot",  "Per class:");
    for (uint8 cls = CLASS_WARRIOR; cls < MAX_CLASSES; ++cls)
    {
        if (perClass[cls])
            TC_LOG_INFO("playerbot",  "    {}: {}", ChatHelper::formatClass(cls).c_str(), perClass[cls]);
    }
    TC_LOG_INFO("playerbot",  "Per role:");
    TC_LOG_INFO("playerbot",  "    tank: {}", tank);
    TC_LOG_INFO("playerbot",  "    heal: {}", heal);
    TC_LOG_INFO("playerbot",  "    dps: {}", dps);
}

double RandomPlayerbotMgr::GetBuyMultiplier(Player* bot)
{
    uint32 id = bot->GetGUID().GetCounter();
    uint32 value = GetEventValue(id, "buymultiplier");
    if (!value)
    {
        value = urand(1, 120);
        uint32 validIn = urand(sPlayerbotAIConfig.minRandomBotsPriceChangeInterval, sPlayerbotAIConfig.maxRandomBotsPriceChangeInterval);
        SetEventValue(id, "buymultiplier", value, validIn);
    }

    return (double)value / 100.0;
}

double RandomPlayerbotMgr::GetSellMultiplier(Player* bot)
{
    uint32 id = bot->GetGUID().GetCounter();
    uint32 value = GetEventValue(id, "sellmultiplier");
    if (!value)
    {
        value = urand(80, 250);
        uint32 validIn = urand(sPlayerbotAIConfig.minRandomBotsPriceChangeInterval, sPlayerbotAIConfig.maxRandomBotsPriceChangeInterval);
        SetEventValue(id, "sellmultiplier", value, validIn);
    }

    return (double)value / 100.0;
}

uint32 RandomPlayerbotMgr::GetLootAmount(Player* bot)
{
    uint32 id = bot->GetGUID().GetCounter();
    return GetEventValue(id, "lootamount");
}

void RandomPlayerbotMgr::SetLootAmount(Player* bot, uint32 value)
{
    uint32 id = bot->GetGUID().GetCounter();
    SetEventValue(id, "lootamount", value, 24 * 3600);
}

uint32 RandomPlayerbotMgr::GetTradeDiscount(Player* bot)
{
    Group* group = bot->GetGroup();
    return GetLootAmount(bot) / (group ? group->GetMembersCount() : 10);
}

string RandomPlayerbotMgr::HandleRemoteCommand(string request)
{
    string::iterator pos = find(request.begin(), request.end(), ',');
    if (pos == request.end())
    {
        ostringstream out; out << "invalid request: " << request;
        return out.str();
    }

    string command = string(request.begin(), pos);
    ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(uint64(atoi(string(pos + 1, request.end()).c_str())));
    Player* bot = GetPlayerBot(guid);
    if (!bot)
        return "invalid guid";

    PlayerbotAI *ai = bot->GetPlayerbotAI();
    if (!ai)
        return "invalid guid";

    return ai->HandleRemoteCommand(command);
}
