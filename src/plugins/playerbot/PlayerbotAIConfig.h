#pragma once

#include "Configuration/Config.h"
#include "SharedDefines.h"

class Player;
class PlayerbotMgr;
class ChatHandler;

class PlayerbotAIConfig
{
public:
    PlayerbotAIConfig();
    static PlayerbotAIConfig& instance()
    {
        static PlayerbotAIConfig instance;
        return instance;
    }

public:
    bool Initialize();
    bool IsInRandomAccountList(uint32 id);
    bool IsInRandomQuestItemList(uint32 id);
    uint32 GetRandomChangeRange(double scale) const;

    bool enabled;
    bool allowGuildBots;
    uint32 globalCoolDown, reactDelay, maxWaitForMove;
    float sightDistance, spellDistance, reactDistance, grindDistance, lootDistance,
        fleeDistance, tooCloseDistance, meleeDistance, followDistance, whisperDistance, contactDistance;
    uint32 criticalHealth, lowHealth, mediumHealth, almostFullHealth;
    uint32 lowMana, mediumMana;

    bool randomBotAutologin;
    std::string randomBotMapsAsString;
    std::vector<uint32> randomBotMaps;
    std::list<uint32> randomBotQuestItems;
    std::list<uint32> randomBotAccounts;
    std::list<uint32> randomBotSpellIds;
    uint32 randomBotTeleportDistance;
    float randomGearLoweringChance;
    float randomBotMaxLevelChance;
    uint32 minRandomBots, maxRandomBots;
    uint32 randomBotUpdateInterval, randomBotCountChangeMinInterval, randomBotCountChangeMaxInterval;
    uint32 minRandomBotInWorldTime, maxRandomBotInWorldTime;
    uint32 minRandomBotRandomizeTime, maxRandomBotRandomizeTime;
    uint32 minRandomBotReviveTime, maxRandomBotReviveTime;
    uint32 minRandomBotPvpTime, maxRandomBotPvpTime;
    uint32 minRandomBotsPerInterval, maxRandomBotsPerInterval;
    uint32 minRandomBotsPriceChangeInterval, maxRandomBotsPriceChangeInterval;
    bool randomBotJoinLfg;
    bool randomBotLoginAtStartup;
    uint32 randomBotTeleLevel;
    bool logInGroupOnly, logValuesPerTick;
    // 0 = silent, 1 = report a bot that is stuck in an attack stance and say
    // which gate is holding it, 2 = also log a full combat snapshot once per
    // second per bot (debug level, needs Logger.playerbot=2).
    uint32 debugCombat = 1;      // also the value used when the whole system is disabled
    bool fleeingEnabled;
    std::string combatStrategies, nonCombatStrategies;
    std::string randomBotCombatStrategies, randomBotNonCombatStrategies;
    uint32 randomBotMinLevel, randomBotMaxLevel;
    float randomChangeMultiplier;
    uint32 specProbability[MAX_CLASSES][3];
    std::string commandPrefix;
    std::string randomBotAccountPrefix;
    uint32 randomBotAccountCount;
    bool deleteRandomBotAccounts;
    uint32 randomBotGuildCount;
    bool deleteRandomBotGuilds;
    std::list<uint32> randomBotGuilds;

    bool guildTaskEnabled;
    uint32 minGuildTaskChangeTime, maxGuildTaskChangeTime;
    uint32 minGuildTaskAdvertisementTime, maxGuildTaskAdvertisementTime;
    uint32 minGuildTaskRewardTime, maxGuildTaskRewardTime;

    uint32 iterationsPerTick;

    int commandServerPort;

    std::string GetValue(std::string name);
    void SetValue(std::string name, std::string value);

private:
    ConfigMgr* config;
};

#define sPlayerbotAIConfig PlayerbotAIConfig::instance()
