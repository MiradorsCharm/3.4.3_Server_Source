#include "BotState.h"

#include "DatabaseEnv.h"
#include "Log.h"

namespace
{
    bool HasColumn(char const* table, char const* column)
    {
        QueryResult result = CharacterDatabase.PQuery(
            "SELECT COUNT(*) FROM information_schema.COLUMNS "
            "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '{}' AND COLUMN_NAME = '{}'",
            table, column);
        return result && result->Fetch()[0].GetUInt64() > 0;
    }
}

void BotState::EnsureSchema()
{
    CharacterDatabase.Execute(
        "CREATE TABLE IF NOT EXISTS characters_playerbot ("
        "guid INT UNSIGNED NOT NULL,"
        "master INT UNSIGNED NOT NULL DEFAULT 0,"
        "random_bot TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "tank_mode TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "grind_mode TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "stay TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "stay_x FLOAT NOT NULL DEFAULT 0,"
        "stay_y FLOAT NOT NULL DEFAULT 0,"
        "stay_z FLOAT NOT NULL DEFAULT 0,"
        "prepared_level TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "last_seen INT UNSIGNED NOT NULL DEFAULT 0,"
        "PRIMARY KEY(guid)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");

    // Tables created by an older build carry only (guid, master). Add the rest
    // in place - MySQL has no "ADD COLUMN IF NOT EXISTS", so ask the
    // information schema first.
    static std::pair<char const*, char const*> const columns[] =
    {
        { "master",         "INT UNSIGNED NOT NULL DEFAULT 0" },
        { "random_bot",     "TINYINT UNSIGNED NOT NULL DEFAULT 0" },
        { "tank_mode",      "TINYINT UNSIGNED NOT NULL DEFAULT 0" },
        { "grind_mode",     "TINYINT UNSIGNED NOT NULL DEFAULT 0" },
        { "stay",           "TINYINT UNSIGNED NOT NULL DEFAULT 0" },
        { "stay_x",         "FLOAT NOT NULL DEFAULT 0" },
        { "stay_y",         "FLOAT NOT NULL DEFAULT 0" },
        { "stay_z",         "FLOAT NOT NULL DEFAULT 0" },
        { "prepared_level", "TINYINT UNSIGNED NOT NULL DEFAULT 0" },
        { "last_seen",      "INT UNSIGNED NOT NULL DEFAULT 0" },
    };

    for (auto const& [name, definition] : columns)
    {
        if (HasColumn("characters_playerbot", name))
            continue;
        CharacterDatabase.PExecute("ALTER TABLE characters_playerbot ADD COLUMN {} {}", name, definition);
        TC_LOG_INFO("playerbot", "characters_playerbot: added column {}", name);
    }
}

bool BotState::LoadAll(std::vector<BotSavedState>& out)
{
    out.clear();

    QueryResult result = CharacterDatabase.Query(
        "SELECT guid, master, random_bot, tank_mode, grind_mode, stay, "
        "stay_x, stay_y, stay_z, prepared_level, last_seen "
        "FROM characters_playerbot ORDER BY last_seen ASC, guid ASC");
    if (!result)
        return false;

    do
    {
        Field* fields = result->Fetch();
        BotSavedState state;
        state.guid = ObjectGuid::Create<HighGuid::Player>(fields[0].GetUInt32());
        if (uint32 master = fields[1].GetUInt32())
            state.master = ObjectGuid::Create<HighGuid::Player>(master);
        state.random = fields[2].GetUInt8() != 0;
        state.tankMode = fields[3].GetUInt8() != 0;
        state.grindMode = fields[4].GetUInt8() != 0;
        state.stay = fields[5].GetUInt8() != 0;
        state.stayX = fields[6].GetFloat();
        state.stayY = fields[7].GetFloat();
        state.stayZ = fields[8].GetFloat();
        state.preparedLevel = fields[9].GetUInt8();
        state.lastSeen = fields[10].GetUInt32();
        state.present = true;
        out.push_back(state);
    } while (result->NextRow());

    return true;
}

BotSavedState BotState::Load(ObjectGuid guid)
{
    BotSavedState state;
    if (guid.IsEmpty())
        return state;

    QueryResult result = CharacterDatabase.PQuery(
        "SELECT guid, master, random_bot, tank_mode, grind_mode, stay, "
        "stay_x, stay_y, stay_z, prepared_level, last_seen "
        "FROM characters_playerbot WHERE guid = {}", guid.GetCounter());
    if (!result)
        return state;

    Field* fields = result->Fetch();
    state.guid = guid;
    if (uint32 master = fields[1].GetUInt32())
        state.master = ObjectGuid::Create<HighGuid::Player>(master);
    state.random = fields[2].GetUInt8() != 0;
    state.tankMode = fields[3].GetUInt8() != 0;
    state.grindMode = fields[4].GetUInt8() != 0;
    state.stay = fields[5].GetUInt8() != 0;
    state.stayX = fields[6].GetFloat();
    state.stayY = fields[7].GetFloat();
    state.stayZ = fields[8].GetFloat();
    state.preparedLevel = fields[9].GetUInt8();
    state.lastSeen = fields[10].GetUInt32();
    state.present = true;
    return state;
}

void BotState::Save(BotSavedState const& state)
{
    if (!state.guid.IsPlayer())
        return;

    // Upsert: a bot added by ".bot add" before any state existed gets its row
    // here, and an existing row keeps whichever columns this call fills.
    CharacterDatabase.PExecute(
        "INSERT INTO characters_playerbot "
        "(guid, master, random_bot, tank_mode, grind_mode, stay, stay_x, stay_y, stay_z, prepared_level, last_seen) "
        "VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}) "
        "ON DUPLICATE KEY UPDATE "
        "master = {}, random_bot = {}, tank_mode = {}, grind_mode = {}, stay = {}, "
        "stay_x = {}, stay_y = {}, stay_z = {}, prepared_level = {}, last_seen = {}",
        state.guid.GetCounter(), state.master.GetCounter(),
        uint32(state.random ? 1 : 0), uint32(state.tankMode ? 1 : 0), uint32(state.grindMode ? 1 : 0),
        uint32(state.stay ? 1 : 0), state.stayX, state.stayY, state.stayZ,
        uint32(state.preparedLevel), state.lastSeen,
        state.master.GetCounter(),
        uint32(state.random ? 1 : 0), uint32(state.tankMode ? 1 : 0), uint32(state.grindMode ? 1 : 0),
        uint32(state.stay ? 1 : 0), state.stayX, state.stayY, state.stayZ,
        uint32(state.preparedLevel), state.lastSeen);
}

void BotState::SetPreparedLevel(ObjectGuid guid, uint8 level)
{
    if (!guid.IsPlayer())
        return;

    CharacterDatabase.PExecute(
        "INSERT INTO characters_playerbot (guid, prepared_level) VALUES ({}, {}) "
        "ON DUPLICATE KEY UPDATE prepared_level = {}",
        guid.GetCounter(), uint32(level), uint32(level));
}

void BotState::SetMaster(ObjectGuid botGuid, ObjectGuid masterGuid)
{
    if (!botGuid.IsPlayer())
        return;

    if (masterGuid.IsEmpty())
    {
        // An unbound bot keeps its row (its role/level bookkeeping survives);
        // only the ownership is cleared.
        CharacterDatabase.PExecute(
            "UPDATE characters_playerbot SET master = 0 WHERE guid = {}", botGuid.GetCounter());
        return;
    }

    CharacterDatabase.PExecute(
        "INSERT INTO characters_playerbot (guid, master) VALUES ({}, {}) "
        "ON DUPLICATE KEY UPDATE master = {}",
        botGuid.GetCounter(), masterGuid.GetCounter(), masterGuid.GetCounter());
}

void BotState::Forget(ObjectGuid guid)
{
    if (guid.IsEmpty())
        return;
    CharacterDatabase.PExecute("DELETE FROM characters_playerbot WHERE guid = {}", guid.GetCounter());
}

uint32 BotState::Count()
{
    QueryResult result = CharacterDatabase.Query("SELECT COUNT(*) FROM characters_playerbot");
    return result ? result->Fetch()[0].GetUInt32() : 0;
}
