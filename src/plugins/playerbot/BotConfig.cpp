#include "BotConfig.h"

#include "Config.h"
#include "Log.h"

#include <algorithm>

bool BotConfig::Load()
{
    Enabled = sConfigMgr->GetBoolDefault("AiPlayerbot.Enabled", true);
    if (!Enabled)
        return false;

    auto getInt = [this](char const* key, uint32 def)
    {
        int32 v = sConfigMgr->GetIntDefault(key, int32(def));
        return v > 0 ? uint32(v) : 0;
    };
    auto getFloat = [this](char const* key, float def)
    {
        float v = sConfigMgr->GetFloatDefault(key, def);
        return v > 0.0f ? v : def;
    };

    Diagnostics = sConfigMgr->GetBoolDefault("AiPlayerbot.Diagnostics", true);
    StallReportMs = getInt("AiPlayerbot.StallReportMs", 4000);
    if (StallReportMs < 1000)
        StallReportMs = 1000;
    StallReportCooldownMs = getInt("AiPlayerbot.StallReportCooldownMs", 10000);
    DebugMove = sConfigMgr->GetBoolDefault("AiPlayerbot.DebugMove", false);

    SightDistance = getFloat("AiPlayerbot.SightDistance", 50.0f);
    SpellDistance = getFloat("AiPlayerbot.SpellDistance", 25.0f);
    LootDistance = getFloat("AiPlayerbot.LootDistance", 25.0f);
    FollowDistance = getFloat("AiPlayerbot.FollowDistance", 4.0f);
    WanderRadius = getFloat("AiPlayerbot.WanderRadius", 20.0f);
    MeleeStopFactor = sConfigMgr->GetFloatDefault("AiPlayerbot.MeleeStopFactor", 0.8f);
    if (MeleeStopFactor <= 0.1f || MeleeStopFactor > 0.95f)
        MeleeStopFactor = 0.8f;
    CastStandDistance = getFloat("AiPlayerbot.CastStandDistance", 18.0f);
    CastMinDistance = getFloat("AiPlayerbot.CastMinDistance", 8.0f);
    if (CastMinDistance > CastStandDistance)
        CastMinDistance = CastStandDistance;

    AutoAssistMaster = sConfigMgr->GetBoolDefault("AiPlayerbot.AutoAssistMaster", true);
    ReviveDelayMs = getInt("AiPlayerbot.ReviveDelayMs", 5000);
    EatDrinkPct = getInt("AiPlayerbot.EatDrinkPct", 50);
    Grind = sConfigMgr->GetBoolDefault("AiPlayerbot.Grind", true);

    AvoidGroundHazards = sConfigMgr->GetBoolDefault("AiPlayerbot.AvoidGroundHazards", true);
    HazardSafetyMargin = sConfigMgr->GetFloatDefault("AiPlayerbot.HazardSafetyMargin", 2.0f);
    if (HazardSafetyMargin < 0.0f || HazardSafetyMargin > 15.0f)
        HazardSafetyMargin = 2.0f;
    InterruptCasts = sConfigMgr->GetBoolDefault("AiPlayerbot.InterruptCasts", true);

    RandomBotCount = getInt("AiPlayerbot.RandomBotCount", 0);
    RandomBotMinLevel = getInt("AiPlayerbot.RandomBotMinLevel", 1);
    RandomBotMaxLevel = getInt("AiPlayerbot.RandomBotMaxLevel", 60);
    if (RandomBotMinLevel > RandomBotMaxLevel)
        RandomBotMinLevel = RandomBotMaxLevel;
    RandomBotUpdateInterval = getInt("AiPlayerbot.RandomBotUpdateInterval", 30);
    RandomBotAccountPrefix = sConfigMgr->GetStringDefault("AiPlayerbot.RandomBotAccountPrefix", "rndbot");
    BotAccountPrefix = RandomBotAccountPrefix;

    LoginStaggerMs = sConfigMgr->GetIntDefault("AiPlayerbot.LoginStaggerMs", int32(LoginStaggerMs));
    if (LoginStaggerMs < 0)
        LoginStaggerMs = 0;
    MaxConcurrentLogins = uint32(std::max<int32>(1, sConfigMgr->GetIntDefault("AiPlayerbot.MaxConcurrentLogins", int32(MaxConcurrentLogins))));
    StartupLoginDelayMs = uint32(std::max<int32>(0, sConfigMgr->GetIntDefault("AiPlayerbot.StartupLoginDelayMs", int32(StartupLoginDelayMs))));

    PersistBots = sConfigMgr->GetBoolDefault("AiPlayerbot.PersistBots", true);
    StateSaveIntervalMs = uint32(std::max<int32>(5000, sConfigMgr->GetIntDefault("AiPlayerbot.StateSaveIntervalMs", int32(StateSaveIntervalMs))));
    RestoreRandomBots = sConfigMgr->GetBoolDefault("AiPlayerbot.RestoreRandomBots", true);

    RandomBotSpread = sConfigMgr->GetBoolDefault("AiPlayerbot.RandomBotSpread", true);
    RandomBotRelocateMinutes = uint32(std::max<int32>(0, sConfigMgr->GetIntDefault("AiPlayerbot.RandomBotRelocateMinutes", 0)));
    RandomBotCreateBatch = uint32(std::max<int32>(1, sConfigMgr->GetIntDefault("AiPlayerbot.RandomBotCreateBatch", int32(RandomBotCreateBatch))));

    HopelessLevelGap = std::clamp<int32>(sConfigMgr->GetIntDefault("AiPlayerbot.HopelessLevelGap", HopelessLevelGap), 0, 60);
    CastRetryMs = uint32(std::clamp<int32>(sConfigMgr->GetIntDefault("AiPlayerbot.CastRetryMs", int32(CastRetryMs)), 50, 2000));

    TC_LOG_INFO("playerbot", "Playerbot AI loaded: {} random bot(s) targeted, diagnostics {}, prefix '{}'",
        RandomBotCount, Diagnostics ? "on" : "off", RandomBotAccountPrefix);
    TC_LOG_INFO("playerbot", "  login pacing: {}ms between logins, {} at a time, {}ms after start; state {}",
        LoginStaggerMs, MaxConcurrentLogins, StartupLoginDelayMs, PersistBots ? "saved" : "not saved");
    return true;
}
