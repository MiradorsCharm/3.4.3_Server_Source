/*
 * Playerbot AI - configuration.
 *
 * Every knob the AI reads lives in the "AI PLAYERBOT SETTINGS" section of
 * worldserver.conf. All values are read once at startup; changing them needs
 * a worldserver restart.
 */

#ifndef PLAYERBOT_BOT_CONFIG_H
#define PLAYERBOT_BOT_CONFIG_H

#include "Define.h"

#include <string>

class BotConfig
{
public:
    static BotConfig* instance()
    {
        static BotConfig cfg;
        return &cfg;
    }

    /// Read AiPlayerbot.* from the worldserver config. Returns false (and logs
    /// why) when the bot system must not start.
    bool Load();

    bool Enabled = true;

    /// Periodic stuck-bot console reports (the "Bot X is locked onto Y" lines).
    bool Diagnostics = true;
    /// A bot that makes no combat progress for this long is reported.
    uint32 StallReportMs = 4000;
    /// Minimum pause between two reports about the same bot.
    uint32 StallReportCooldownMs = 10000;
    /// Very chatty per-tick movement trace, for debugging only.
    bool DebugMove = false;

    /// Distances (yards).
    float SightDistance = 50.0f;     // how far a bot looks for something to fight
    float SpellDistance = 25.0f;     // generic "far enough" spell stand-off
    float LootDistance = 25.0f;      // how far away a corpse is still worth looting
    float FollowDistance = 4.0f;     // following behind the master
    float WanderRadius = 20.0f;      // random bot stroll radius
    float MeleeStopFactor = 0.8f;    // chase stops at this fraction of the live swing range
    float CastStandDistance = 18.0f; // casters try to stand this far from the victim
    float CastMinDistance = 8.0f;    // casters back off when a melee mob is closer than this

    /// Combat behaviour.
    bool AutoAssistMaster = true;    // bots join a group fight started by their master
    uint32 ReviveDelayMs = 5000;     // dead bots resurrect themselves after this long
    uint32 EatDrinkPct = 50;         // bots eat/drink when below this % (out of combat)
    bool Grind = true;               // random bots fight nearby mobs while wandering

    /// Dungeon / raid mechanic awareness.
    bool AvoidGroundHazards = true;  // step out of hostile ground effects (fire, Defile, ...)
    float HazardSafetyMargin = 2.0f; // extra yards kept clear beyond a hazard's radius
    bool InterruptCasts = true;      // interrupt dangerous boss/trash casts when able

    /// Random bot pool.
    uint32 RandomBotCount = 0;       // how many random bots should be online (0 = feature off)
    uint32 RandomBotMinLevel = 1;
    uint32 RandomBotMaxLevel = 60;
    uint32 RandomBotUpdateInterval = 30;   // seconds between pool audits
    std::string RandomBotAccountPrefix = "rndbot";

    /// --- login pacing --------------------------------------------------------
    /// Logging a bot in loads a whole character from the database and (the
    /// first time) prepares it. Doing hundreds of those in one tick is what
    /// used to freeze the world for a minute after every start.
    uint32 LoginStaggerMs = 400;     // minimum pause between two bot logins
    uint32 MaxConcurrentLogins = 3;  // how many logins may be in flight at once
    uint32 StartupLoginDelayMs = 15000;  // grace period after boot before the pool fills

    /// --- persistence ---------------------------------------------------------
    /// Bots remember that they were online, where they stood and what they were
    /// told to do, so a restart brings back the same world instead of a fresh
    /// crowd. Position itself is the core's own (characters.position_*), saved
    /// on logout; this keeps the roster and the AI flags next to it.
    bool PersistBots = true;
    uint32 StateSaveIntervalMs = 60000;    // periodic state save while running
    bool RestoreRandomBots = true;         // bring the saved random pool back on start

    /// --- world spread --------------------------------------------------------
    /// Random bots are placed (and periodically relocated) at level-matching
    /// spots all over the world instead of piling into their starting zones.
    bool RandomBotSpread = true;
    uint32 RandomBotRelocateMinutes = 0;   // 0 = never move a settled bot
    uint32 RandomBotCreateBatch = 5;       // new bot characters created per audit

    /// --- combat sanity -------------------------------------------------------
    /// A bot that cannot hurt its attacker is not "stalled", it is outmatched.
    int32 HopelessLevelGap = 5;      // victim this many levels above us: give up
    uint32 CastRetryMs = 150;        // how often the rotation is re-evaluated

    /// Once per process: characters of accounts with this prefix are bots.
    std::string BotAccountPrefix = "rndbot";

private:
    BotConfig() = default;
};

#define sBotConfig BotConfig::instance()

#endif
