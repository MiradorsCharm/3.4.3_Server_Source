#include "../pchdef.h"
#include "PlayerbotAIConfig.h"
#include "playerbot.h"
#include "RandomPlayerbotFactory.h"
#include "../ahbot/AhBotConfig.h"
#include <charconv>
#include <limits>
#include "Accounts/AccountMgr.h"
#include "../../server/database/Database/DatabaseEnv.h"

using namespace std;

namespace
{
    // Column spec: name + full definition as written in
    // sql/custom/playerbot/characters_playerbot.sql
    struct BotTableColumn
    {
        char const* name;
        char const* definition;
    };

    struct BotTableSpec
    {
        char const* name;
        char const* createSql;
        BotTableColumn const* columns;
        size_t columnCount;
        char const* primaryKeyDef;   // e.g. "(`owner`, `bot`, `event`)" or null
    };

    // Ensures every ai_playerbot_* table exists and has all columns the code
    // expects. CREATE TABLE IF NOT EXISTS alone can never extend a table that
    // already exists, so installs that came from older ai_playerbot_* dumps
    // keep missing columns (e.g. `gender` in ai_playerbot_names, `owner` in
    // the event tables) forever - every query against them then fails at BEST
    // (and used to abort the server). Create missing tables, add missing
    // columns, repair a missing primary key and realign a foreign table
    // collation (the name lookups JOIN these tables against characters/guild,
    // and MySQL refuses mixed-collation comparisons with errno 1267); anything
    // that cannot be added (e.g. a PK that would collapse duplicate legacy
    // rows) is logged as an error instead of crashing.
    void EnsureBotTable(BotTableSpec const& spec)
    {
        CharacterDatabase.PExecute("{}", spec.createSql);

        // Collation self-repair: the JOINs the name pickers run compare
        // ai_playerbot_*.name with characters.name / guild.name. A table that
        // was created before the explicit COLLATE above (or by a database whose
        // default collation is MySQL 8's utf8mb4_0900_ai_ci) makes every one of
        // those JOINs fail with errno 1267 "Illegal mix of collations", which
        // used to surface as "No more names left for random guilds" while
        // plenty of names were left. Convert once, here, instead of leaving the
        // install broken until someone finds the right ALTER by hand.
        QueryResult wrongCollation = CharacterDatabase.PQuery(
                "SELECT 1 FROM information_schema.TABLES "
                "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '{}' "
                "AND TABLE_COLLATION <> 'utf8mb4_unicode_ci'", spec.name);
        if (wrongCollation && wrongCollation->GetRowCount())
        {
            TC_LOG_ERROR("playerbot",
                    "Playerbot table `{}` was created with a foreign collation - "
                    "converting it to utf8mb4_unicode_ci (mismatched collations break "
                    "the name lookups with errno 1267)", spec.name);
            CharacterDatabase.PExecute(
                    "ALTER TABLE `{}` CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci",
                    spec.name);
        }

        for (size_t i = 0; i < spec.columnCount; ++i)
        {
            BotTableColumn const& column = spec.columns[i];
            QueryResult exists = CharacterDatabase.PQuery(
                    "SELECT 1 FROM information_schema.COLUMNS "
                    "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '{}' AND COLUMN_NAME = '{}'",
                    spec.name, column.name);
            // a successful but empty result means the column is missing
            if (exists && exists->GetRowCount())
                continue;

            TC_LOG_ERROR("playerbot",
                    "Playerbot table `{}` is missing the `{}` column - adding it",
                    spec.name, column.name);
            CharacterDatabase.PExecute("ALTER TABLE `{}` ADD COLUMN {} {}", spec.name, column.name, column.definition);
        }

        if (!spec.primaryKeyDef)
            return;

        QueryResult hasPk = CharacterDatabase.PQuery(
                "SELECT 1 FROM information_schema.TABLE_CONSTRAINTS "
                "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '{}' AND CONSTRAINT_TYPE = 'PRIMARY KEY'",
                spec.name);
        if (hasPk && hasPk->GetRowCount())
            return;

        TC_LOG_ERROR("playerbot", "Playerbot table `{}` has no primary key - adding one", spec.name);
        CharacterDatabase.PExecute("ALTER TABLE `{}` ADD PRIMARY KEY {}", spec.name, spec.primaryKeyDef);
    }

    void EnsureBotTables()
    {
        static BotTableColumn const randomBotsColumns[] =
        {
            { "owner",   "INT UNSIGNED NOT NULL DEFAULT 0" },
            { "bot",     "INT UNSIGNED NOT NULL DEFAULT 0" },
            { "time",    "INT UNSIGNED NOT NULL DEFAULT 0" },
            { "validIn", "INT UNSIGNED NOT NULL DEFAULT 0" },
            { "event",   "VARCHAR(64) NOT NULL DEFAULT ''" },
            { "value",   "INT UNSIGNED NOT NULL DEFAULT 0" },
        };

        static BotTableColumn const namesColumns[] =
        {
            { "name_id", "INT UNSIGNED NOT NULL AUTO_INCREMENT" },
            { "name",    "VARCHAR(12) NOT NULL" },
            { "gender",  "TINYINT UNSIGNED NOT NULL DEFAULT 0" },
        };

        static BotTableColumn const guildNamesColumns[] =
        {
            { "name_id", "INT UNSIGNED NOT NULL AUTO_INCREMENT" },
            { "name",    "VARCHAR(24) NOT NULL" },
        };

        static BotTableColumn const guildTasksColumns[] =
        {
            { "owner",   "INT UNSIGNED NOT NULL DEFAULT 0" },
            { "guildid", "INT UNSIGNED NOT NULL DEFAULT 0" },
            { "time",    "INT UNSIGNED NOT NULL DEFAULT 0" },
            { "validIn", "INT UNSIGNED NOT NULL DEFAULT 0" },
            { "type",    "VARCHAR(32) NOT NULL DEFAULT ''" },
            { "value",   "INT UNSIGNED NOT NULL DEFAULT 0" },
        };

        static BotTableColumn const speechColumns[] =
        {
            { "id",   "INT UNSIGNED NOT NULL AUTO_INCREMENT" },
            { "name", "VARCHAR(64) NOT NULL" },
            { "text", "VARCHAR(255) NOT NULL" },
            { "type", "VARCHAR(16) NOT NULL DEFAULT 'say'" },
        };

        static BotTableColumn const speechProbabilityColumns[] =
        {
            { "name",        "VARCHAR(64) NOT NULL" },
            { "probability", "INT UNSIGNED NOT NULL DEFAULT 0" },
        };

        static BotTableColumn const customStrategyColumns[] =
        {
            { "name",        "VARCHAR(64) NOT NULL" },
            { "action_line", "VARCHAR(255) NOT NULL" },
        };

        static BotTableSpec const tables[] =
        {
            { "ai_playerbot_random_bots",
                "CREATE TABLE IF NOT EXISTS `ai_playerbot_random_bots` ("
                "`owner` INT UNSIGNED NOT NULL DEFAULT 0, `bot` INT UNSIGNED NOT NULL DEFAULT 0,"
                "`time` INT UNSIGNED NOT NULL DEFAULT 0, `validIn` INT UNSIGNED NOT NULL DEFAULT 0,"
                "`event` VARCHAR(64) NOT NULL DEFAULT '', `value` INT UNSIGNED NOT NULL DEFAULT 0,"
                "PRIMARY KEY (`owner`, `bot`, `event`), KEY `idx_event` (`event`), KEY `idx_bot` (`bot`)"
                ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci",
                randomBotsColumns, sizeof(randomBotsColumns) / sizeof(randomBotsColumns[0]), "(`owner`, `bot`, `event`)" },
            { "ai_playerbot_names",
                "CREATE TABLE IF NOT EXISTS `ai_playerbot_names` ("
                "`name_id` INT UNSIGNED NOT NULL AUTO_INCREMENT, `name` VARCHAR(12) NOT NULL,"
                "`gender` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
                "PRIMARY KEY (`name_id`), UNIQUE KEY `idx_name` (`name`)"
                ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci",
                namesColumns, sizeof(namesColumns) / sizeof(namesColumns[0]), nullptr },
            { "ai_playerbot_guild_names",
                "CREATE TABLE IF NOT EXISTS `ai_playerbot_guild_names` ("
                "`name_id` INT UNSIGNED NOT NULL AUTO_INCREMENT, `name` VARCHAR(24) NOT NULL,"
                "PRIMARY KEY (`name_id`), UNIQUE KEY `idx_name` (`name`)"
                ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci",
                guildNamesColumns, sizeof(guildNamesColumns) / sizeof(guildNamesColumns[0]), nullptr },
            { "ai_playerbot_guild_tasks",
                "CREATE TABLE IF NOT EXISTS `ai_playerbot_guild_tasks` ("
                "`owner` INT UNSIGNED NOT NULL DEFAULT 0, `guildid` INT UNSIGNED NOT NULL DEFAULT 0,"
                "`time` INT UNSIGNED NOT NULL DEFAULT 0, `validIn` INT UNSIGNED NOT NULL DEFAULT 0,"
                "`type` VARCHAR(32) NOT NULL DEFAULT '', `value` INT UNSIGNED NOT NULL DEFAULT 0,"
                "PRIMARY KEY (`owner`, `guildid`, `type`), KEY `idx_guild` (`guildid`)"
                ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci",
                guildTasksColumns, sizeof(guildTasksColumns) / sizeof(guildTasksColumns[0]), "(`owner`, `guildid`, `type`)" },
            { "ai_playerbot_speech",
                "CREATE TABLE IF NOT EXISTS `ai_playerbot_speech` ("
                "`id` INT UNSIGNED NOT NULL AUTO_INCREMENT, `name` VARCHAR(64) NOT NULL,"
                "`text` VARCHAR(255) NOT NULL, `type` VARCHAR(16) NOT NULL DEFAULT 'say',"
                "PRIMARY KEY (`id`), KEY `idx_name` (`name`)"
                ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci",
                speechColumns, sizeof(speechColumns) / sizeof(speechColumns[0]), nullptr },
            { "ai_playerbot_speech_probability",
                "CREATE TABLE IF NOT EXISTS `ai_playerbot_speech_probability` ("
                "`name` VARCHAR(64) NOT NULL, `probability` INT UNSIGNED NOT NULL DEFAULT 0,"
                "PRIMARY KEY (`name`)"
                ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci",
                speechProbabilityColumns, sizeof(speechProbabilityColumns) / sizeof(speechProbabilityColumns[0]), "(`name`)" },
            { "ai_playerbot_custom_strategy",
                "CREATE TABLE IF NOT EXISTS `ai_playerbot_custom_strategy` ("
                "`name` VARCHAR(64) NOT NULL, `action_line` VARCHAR(255) NOT NULL,"
                "PRIMARY KEY (`name`, `action_line`)"
                ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci",
                customStrategyColumns, sizeof(customStrategyColumns) / sizeof(customStrategyColumns[0]), "(`name`, `action_line`)" },
        };

        for (BotTableSpec const& table : tables)
            EnsureBotTable(table);

        // seed tables with the starter rows from characters_playerbot.sql so
        // bots can be created even on a completely fresh install (synchronous:
        // the rows must be present before CreateRandomBots runs below)
        CharacterDatabase.PExecute(
            "INSERT IGNORE INTO `ai_playerbot_names` (`name`) VALUES"
            "('Aeltar'), ('Baldrin'), ('Cathmor'), ('Dornan'), ('Eldrik'), ('Faelan'),"
            "('Gorvin'), ('Halbrik'), ('Ithran'), ('Jorlan'), ('Kelvar'), ('Lorwyn'),"
            "('Mordak'), ('Nyrelle'), ('Orwin'), ('Perrin'), ('Quenna'), ('Rhogar'),"
            "('Sylvara'), ('Torvald'), ('Ulther'), ('Varlen'), ('Wyndel'), ('Xanthe'),"
            "('Yorik'), ('Zaltar'), ('Ashwyn'), ('Brannoc'), ('Cirien'), ('Draveth'),"
            "('Elowen'), ('Fenwick'), ('Gwynor'), ('Harlow'), ('Isolde'), ('Jareth'),"
            "('Kaelith'), ('Lyanna'), ('Merrick'), ('Norwyn'), ('Ondrel'), ('Pellan'),"
            "('Rowena'), ('Selwyn'), ('Thalric'), ('Ulmara'), ('Verrik'), ('Wilrun'),"
            "('Yalira'), ('Zeryth')");
        CharacterDatabase.PExecute(
            "INSERT IGNORE INTO `ai_playerbot_guild_names` (`name`) VALUES"
            "('The Wandering Blades'), ('Sons of Lordaeron'), ('Emerald Vanguard'),"
            "('Ashen Company'), ('Stormwatch'), ('The Silver Hand Irregulars'),"
            "('Dawnbreakers'), ('Ironforge Regulars'), ('Nightfall Covenant'),"
            "('The Last Caravan')");
    }
}

PlayerbotAIConfig::PlayerbotAIConfig() : config(ConfigMgr::instance())
{
}

template <class T>
void LoadList(string const& value, T& ids, char const* setting)
{
    ids.clear();
    istringstream input(value);
    for (string token; getline(input, token, ',');)
    {
        size_t first = token.find_first_not_of(" \t\r\n");
        if (first == string::npos)
            continue;
        size_t last = token.find_last_not_of(" \t\r\n") + 1;

        uint32 id = 0;
        auto result = std::from_chars(token.data() + first, token.data() + last, id);
        if (result.ec != std::errc() || result.ptr != token.data() + last)
        {
            TC_LOG_ERROR("playerbot", "PlayerbotAIConfig: invalid unsigned ID '{}' in {}; skipping", token, setting);
            continue;
        }

        // Zero is valid (notably map 0). Do not duplicate entries on reload.
        if (find(ids.begin(), ids.end(), id) == ids.end())
            ids.push_back(id);
    }
}

static void LoadRandomBotMaps(string& value, vector<uint32>& maps)
{
    LoadList(value, maps, "AiPlayerbot.RandomBotMaps");
    maps.erase(remove_if(maps.begin(), maps.end(), [](uint32 id)
    {
        MapEntry const* map = sMapStore.LookupEntry(id);
        if (!map || map->Instanceable() || id > numeric_limits<uint16>::max())
        {
            TC_LOG_ERROR("playerbot", "PlayerbotAIConfig: random bot map {} is missing, instanced or out of range; skipping", id);
            return true;
        }
        return false;
    }), maps.end());

    // This string is interpolated into an SQL IN clause. Never pass raw config
    // text to the database: malformed/empty lists can cause fatal SQL errors.
    ostringstream sql;
    for (uint32 id : maps)
    {
        if (sql.tellp() > 0)
            sql << ',';
        sql << id;
    }
    value = sql.str();
}

uint32 PlayerbotAIConfig::GetRandomChangeRange(double scale) const
{
    double multiplier = randomChangeMultiplier;
    if (!std::isfinite(multiplier) || multiplier <= 0.0)
        multiplier = 1.0;

    // A small positive multiplier is valid, but its reciprocal can exceed the
    // integer range used by rand/urand. Saturate before converting to uint32.
    double range = 1.0 + std::max(0.0, scale) / multiplier;
    return uint32(std::clamp(range, 1.0, double(numeric_limits<uint32>::max())));
}

bool PlayerbotAIConfig::Initialize()
{
    // Main.cpp has already loaded worldserver.conf and any explicit overrides.
    // Do not load a separate bot file here: a stale file in the working directory
    // would silently override the settings (and environment overrides) from Main.
    enabled = config->GetBoolDefault("AiPlayerbot.Enabled", true);
    if (!enabled)
    {
        TC_LOG_INFO("playerbot", "AI Playerbot is disabled in {}", config->GetFilename());
        return false;
    }

    TC_LOG_INFO("playerbot",  "Initializing AI Playerbot by ike3, based on the original Playerbot by blueboy");

    // self-heal the ai_playerbot_* tables before anything queries them:
    // create missing tables, add columns that older ai_playerbot_* dumps are
    // missing (IF NOT EXISTS scripts can never upgrade existing tables), and
    // seed the starter name pools on a fresh install
    EnsureBotTables();
    sAhBotConfig.Initialize();

    // Negative unsigned settings must not wrap into billion-sized counts/timers.
    globalCoolDown = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.GlobalCooldown", 500)));
    maxWaitForMove = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.MaxWaitForMove", 3000)));
    reactDelay = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.ReactDelay", 100)));

    sightDistance = config->GetFloatDefault("AiPlayerbot.SightDistance", 50.0f);
    spellDistance = config->GetFloatDefault("AiPlayerbot.SpellDistance", 25.0f);
    reactDistance = config->GetFloatDefault("AiPlayerbot.ReactDistance", 150.0f);
    grindDistance = config->GetFloatDefault("AiPlayerbot.GrindDistance", 100.0f);
    lootDistance = config->GetFloatDefault("AiPlayerbot.LootDistance", 20.0f);
    fleeDistance = config->GetFloatDefault("AiPlayerbot.FleeDistance", 20.0f);
    tooCloseDistance = config->GetFloatDefault("AiPlayerbot.TooCloseDistance", 5.0f);
    // Real melee reach is ~5 yards center-to-center (ATTACK_DISTANCE); the old
    // 0.5 default made bots chase to a point inside the target's model and
    // kept "enemy out of melee" (and its chase action, which outranks "set
    // facing") active even at perfect striking range. 3.0 plants them solidly
    // inside swing range with margin for 2D-vs-3D distance and slopes.
    meleeDistance = config->GetFloatDefault("AiPlayerbot.MeleeDistance", 3.0f);
    // Existing worldserver.conf files generated from the old .dist still
    // carry 0.5: any sub-1.0 value puts the chase destination inside the
    // target's collision and reintroduces the never-plant-never-swing
    // shuffle, so refuse it loudly instead of obeying it.
    if (meleeDistance < 1.0f)
    {
        TC_LOG_ERROR("playerbot", "PlayerbotAIConfig: AiPlayerbot.MeleeDistance ({}) is below 1.0 and would park bots inside their target; using 3.0 instead",
                meleeDistance);
        meleeDistance = 3.0f;
    }
    followDistance = config->GetFloatDefault("AiPlayerbot.FollowDistance", 1.5f);
    whisperDistance = config->GetFloatDefault("AiPlayerbot.WhisperDistance", 6000.0f);
    contactDistance = config->GetFloatDefault("AiPlayerbot.ContactDistance", 0.5f);

    criticalHealth = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.CriticalHealth", 20)));
    lowHealth = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.LowHealth", 50)));
    mediumHealth = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.MediumHealth", 70)));
    almostFullHealth = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.AlmostFullHealth", 85)));
    lowMana = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.LowMana", 15)));
    mediumMana = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.MediumMana", 40)));

    randomGearLoweringChance = config->GetFloatDefault("AiPlayerbot.RandomGearLoweringChance", 0.15);
    randomBotMaxLevelChance = config->GetFloatDefault("AiPlayerbot.RandomBotMaxLevelChance", 0.4);

    iterationsPerTick = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.IterationsPerTick", 10)));

    allowGuildBots = config->GetBoolDefault("AiPlayerbot.AllowGuildBots", true);

    randomBotMapsAsString = config->GetStringDefault("AiPlayerbot.RandomBotMaps", "0,1,530,571");
    LoadRandomBotMaps(randomBotMapsAsString, randomBotMaps);
    LoadList<list<uint32> >(config->GetStringDefault("AiPlayerbot.RandomBotQuestItems", "6948,5175,5176,5177,5178"), randomBotQuestItems, "AiPlayerbot.RandomBotQuestItems");
    LoadList<list<uint32> >(config->GetStringDefault("AiPlayerbot.RandomBotSpellIds", "54197"), randomBotSpellIds, "AiPlayerbot.RandomBotSpellIds");

    randomBotAutologin = config->GetBoolDefault("AiPlayerbot.RandomBotAutologin", true);
    minRandomBots = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.MinRandomBots", 50)));
    maxRandomBots = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.MaxRandomBots", 200)));
    randomBotUpdateInterval = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.RandomBotUpdateInterval", 60)));
    randomBotCountChangeMinInterval = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.RandomBotCountChangeMinInterval", 24 * 3600)));
    randomBotCountChangeMaxInterval = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.RandomBotCountChangeMaxInterval", 3 * 24 * 3600)));
    minRandomBotInWorldTime = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.MinRandomBotInWorldTime", 24 * 3600)));
    maxRandomBotInWorldTime = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.MaxRandomBotInWorldTime", 14 * 24 * 3600)));
    minRandomBotRandomizeTime = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.MinRandomBotRandomizeTime", 2 * 3600)));
    maxRandomBotRandomizeTime = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.MaxRandomRandomizeTime", 14 * 24 * 3600)));
    minRandomBotReviveTime = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.MinRandomBotReviveTime", 60)));
    maxRandomBotReviveTime = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.MaxRandomReviveTime", 300)));
    randomBotTeleportDistance = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.RandomBotTeleportDistance", 1000)));
    minRandomBotsPerInterval = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.MinRandomBotsPerInterval", 50)));
    maxRandomBotsPerInterval = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.MaxRandomBotsPerInterval", 100)));
    minRandomBotsPriceChangeInterval = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.MinRandomBotsPriceChangeInterval", 2 * 3600)));
    maxRandomBotsPriceChangeInterval = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.MaxRandomBotsPriceChangeInterval", 48 * 3600)));
    randomBotJoinLfg = config->GetBoolDefault("AiPlayerbot.RandomBotJoinLfg", true);
    logInGroupOnly = config->GetBoolDefault("AiPlayerbot.LogInGroupOnly", true);
    logValuesPerTick = config->GetBoolDefault("AiPlayerbot.LogValuesPerTick", false);
    debugCombat = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.DebugCombat", 1)));
    if (debugCombat > 2)
    {
        // An out-of-range value is a typo, not a request to log every tick of every
        // bot on the realm; clamp and say so (WARN, not ERROR: it is self-correcting).
        TC_LOG_WARN("playerbot", "PlayerbotAIConfig: AiPlayerbot.DebugCombat ({}) is above 2; using 2", debugCombat);
        debugCombat = 2;
    }
    fleeingEnabled = config->GetBoolDefault("AiPlayerbot.FleeingEnabled", true);
    randomBotMinLevel = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.RandomBotMinLevel", 1)));
    randomBotMaxLevel = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.RandomBotMaxLevel", 255)));
    randomBotLoginAtStartup = config->GetBoolDefault("AiPlayerbot.RandomBotLoginAtStartup", true);
    randomBotTeleLevel = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.RandomBotTeleLevel", 3)));

    randomChangeMultiplier = config->GetFloatDefault("AiPlayerbot.RandomChangeMultiplier", 1.0);

    randomBotCombatStrategies = config->GetStringDefault("AiPlayerbot.RandomBotCombatStrategies", "+dps,+dps assist,-threat");
    randomBotNonCombatStrategies = config->GetStringDefault("AiPlayerbot.RandomBotNonCombatStrategies", "+grind,+move random,+loot");
    combatStrategies = config->GetStringDefault("AiPlayerbot.CombatStrategies", "+custom::say");
    nonCombatStrategies = config->GetStringDefault("AiPlayerbot.NonCombatStrategies", "+custom::say");

    commandPrefix = config->GetStringDefault("AiPlayerbot.CommandPrefix", "");

    commandServerPort = config->GetIntDefault("AiPlayerbot.CommandServerPort", 0);

    for (uint32 cls = 0; cls < MAX_CLASSES; ++cls)
    {
        for (uint32 spec = 0; spec < 3; ++spec)
        {
            specProbability[cls][spec] = 33;

            // SharedDefines also contains retail-only classes. Do not request
            // settings for class 0, Monk, Demon Hunter, Evoker or Adventurer in Wrath.
            if (cls == CLASS_NONE || cls == CLASS_MONK || cls > CLASS_DRUID)
                continue;

            ostringstream os; os << "AiPlayerbot.RandomClassSpecProbability." << cls << "." << spec;
            int32 weight = config->GetIntDefault(os.str(), 33);
            if (weight < 0)
                TC_LOG_ERROR("playerbot", "PlayerbotAIConfig: {} ({}) cannot be negative; using 0", os.str(), weight);
            specProbability[cls][spec] = uint32(std::max(0, weight));
        }
    }

    randomBotAccountPrefix = config->GetStringDefault("AiPlayerbot.RandomBotAccountPrefix", "rndbot");
    randomBotAccountCount = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.RandomBotAccountCount", 50)));
    deleteRandomBotAccounts = config->GetBoolDefault("AiPlayerbot.DeleteRandomBotAccounts", false);
    randomBotGuildCount = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.RandomBotGuildCount", 50)));
    deleteRandomBotGuilds = config->GetBoolDefault("AiPlayerbot.DeleteRandomBotGuilds", false);

    guildTaskEnabled = config->GetBoolDefault("AiPlayerbot.EnableGuildTasks", true);
    minGuildTaskChangeTime = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.MinGuildTaskChangeTime", 2 * 24 * 3600)));
    maxGuildTaskChangeTime = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.MaxGuildTaskChangeTime", 5 * 24 * 3600)));
    minGuildTaskAdvertisementTime = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.MinGuildTaskAdvertisementTime", 8 * 3600)));
    maxGuildTaskAdvertisementTime = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.MaxGuildTaskAdvertisementTime", 4 * 24 * 3600)));
    minGuildTaskRewardTime = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.MinGuildTaskRewardTime", 60)));
    maxGuildTaskRewardTime = uint32(std::max(0, config->GetIntDefault("AiPlayerbot.MaxGuildTaskRewardTime", 600)));

    // An inverted min/max pair from the config would eventually reach
    // urand(min, max) with max < min, whose ASSERT(max >= min) crashes the
    // worldserver - validate every pair and swap inverted values instead of
    // trusting the configuration file blindly
    auto normalizeRange = [](const char* minName, const char* maxName, uint32& minValue, uint32& maxValue)
    {
        if (minValue > maxValue)
        {
            TC_LOG_ERROR("playerbot", "PlayerbotAIConfig: configuration value {} ({}) is greater than {} ({}); swapping the two values",
                    minName, minValue, maxName, maxValue);
            uint32 temp = minValue;
            minValue = maxValue;
            maxValue = temp;
        }
    };

    normalizeRange("AiPlayerbot.MinRandomBots", "AiPlayerbot.MaxRandomBots", minRandomBots, maxRandomBots);
    normalizeRange("AiPlayerbot.RandomBotCountChangeMinInterval", "AiPlayerbot.RandomBotCountChangeMaxInterval",
            randomBotCountChangeMinInterval, randomBotCountChangeMaxInterval);
    normalizeRange("AiPlayerbot.MinRandomBotsPerInterval", "AiPlayerbot.MaxRandomBotsPerInterval",
            minRandomBotsPerInterval, maxRandomBotsPerInterval);
    normalizeRange("AiPlayerbot.MinRandomBotInWorldTime", "AiPlayerbot.MaxRandomBotInWorldTime",
            minRandomBotInWorldTime, maxRandomBotInWorldTime);
    normalizeRange("AiPlayerbot.MinRandomBotRandomizeTime", "AiPlayerbot.MaxRandomRandomizeTime",
            minRandomBotRandomizeTime, maxRandomBotRandomizeTime);
    normalizeRange("AiPlayerbot.MinRandomBotReviveTime", "AiPlayerbot.MaxRandomReviveTime",
            minRandomBotReviveTime, maxRandomBotReviveTime);
    normalizeRange("AiPlayerbot.MinRandomBotPvpTime", "AiPlayerbot.MaxRandomBotPvpTime",
            minRandomBotPvpTime, maxRandomBotPvpTime);
    normalizeRange("AiPlayerbot.MinRandomBotsPriceChangeInterval", "AiPlayerbot.MaxRandomBotsPriceChangeInterval",
            minRandomBotsPriceChangeInterval, maxRandomBotsPriceChangeInterval);
    normalizeRange("AiPlayerbot.RandomBotMinLevel", "AiPlayerbot.RandomBotMaxLevel", randomBotMinLevel, randomBotMaxLevel);
    normalizeRange("AiPlayerbot.MinGuildTaskChangeTime", "AiPlayerbot.MaxGuildTaskChangeTime",
            minGuildTaskChangeTime, maxGuildTaskChangeTime);
    normalizeRange("AiPlayerbot.MinGuildTaskAdvertisementTime", "AiPlayerbot.MaxGuildTaskAdvertisementTime",
            minGuildTaskAdvertisementTime, maxGuildTaskAdvertisementTime);
    normalizeRange("AiPlayerbot.MinGuildTaskRewardTime", "AiPlayerbot.MaxGuildTaskRewardTime",
            minGuildTaskRewardTime, maxGuildTaskRewardTime);

    // used as a divisor in trigger/LFG probability rolls - zero (or a negative
    // value) makes the division produce inf/garbage and undefined int casts
    if (!std::isfinite(randomChangeMultiplier) || randomChangeMultiplier <= 0.0f)
    {
        TC_LOG_ERROR("playerbot", "PlayerbotAIConfig: AiPlayerbot.RandomChangeMultiplier ({}) must be positive; using 1.0",
                randomChangeMultiplier);
        randomChangeMultiplier = 1.0f;
    }

    // Millisecond conversion and the scheduling multiplications must not wrap.
    if (!randomBotUpdateInterval || randomBotUpdateInterval > numeric_limits<uint32>::max() / 1000)
    {
        TC_LOG_ERROR("playerbot", "PlayerbotAIConfig: AiPlayerbot.RandomBotUpdateInterval ({}) is out of range; using 60 seconds",
            randomBotUpdateInterval);
        randomBotUpdateInterval = 60;
    }

    // This prefix participates in account SQL, including optional deletion.
    // Empty/wildcard/quoted prefixes must not broaden that operation or break SQL.
    if (randomBotAccountPrefix.empty() || !all_of(randomBotAccountPrefix.begin(), randomBotAccountPrefix.end(), [](char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    }))
    {
        TC_LOG_ERROR("playerbot", "PlayerbotAIConfig: AiPlayerbot.RandomBotAccountPrefix must contain only ASCII letters, digits or underscores and cannot be empty; disabling bots");
        enabled = false;
        return false;
    }

    RandomPlayerbotFactory::CreateRandomBots();
    TC_LOG_INFO("playerbot", "AI Playerbot configuration loaded from {}", config->GetFilename());

    return true;
}


bool PlayerbotAIConfig::IsInRandomAccountList(uint32 id)
{
    return find(randomBotAccounts.begin(), randomBotAccounts.end(), id) != randomBotAccounts.end();
}

bool PlayerbotAIConfig::IsInRandomQuestItemList(uint32 id)
{
    return find(randomBotQuestItems.begin(), randomBotQuestItems.end(), id) != randomBotQuestItems.end();
}

string PlayerbotAIConfig::GetValue(string name)
{
    ostringstream out;

    if (name == "GlobalCooldown")
        out << globalCoolDown;
    else if (name == "ReactDelay")
        out << reactDelay;

    else if (name == "SightDistance")
        out << sightDistance;
    else if (name == "SpellDistance")
        out << spellDistance;
    else if (name == "ReactDistance")
        out << reactDistance;
    else if (name == "GrindDistance")
        out << grindDistance;
    else if (name == "LootDistance")
        out << lootDistance;
    else if (name == "FleeDistance")
        out << fleeDistance;

    else if (name == "CriticalHealth")
        out << criticalHealth;
    else if (name == "LowHealth")
        out << lowHealth;
    else if (name == "MediumHealth")
        out << mediumHealth;
    else if (name == "AlmostFullHealth")
        out << almostFullHealth;
    else if (name == "LowMana")
        out << lowMana;

    else if (name == "IterationsPerTick")
        out << iterationsPerTick;

    return out.str();
}

void PlayerbotAIConfig::SetValue(string name, string value)
{
    istringstream out(value, istringstream::in);

    if (name == "GlobalCooldown")
        out >> globalCoolDown;
    else if (name == "ReactDelay")
        out >> reactDelay;

    else if (name == "SightDistance")
        out >> sightDistance;
    else if (name == "SpellDistance")
        out >> spellDistance;
    else if (name == "ReactDistance")
        out >> reactDistance;
    else if (name == "GrindDistance")
        out >> grindDistance;
    else if (name == "LootDistance")
        out >> lootDistance;
    else if (name == "FleeDistance")
        out >> fleeDistance;

    else if (name == "CriticalHealth")
        out >> criticalHealth;
    else if (name == "LowHealth")
        out >> lowHealth;
    else if (name == "MediumHealth")
        out >> mediumHealth;
    else if (name == "AlmostFullHealth")
        out >> almostFullHealth;
    else if (name == "LowMana")
        out >> lowMana;

    else if (name == "IterationsPerTick")
        out >> iterationsPerTick;
}
