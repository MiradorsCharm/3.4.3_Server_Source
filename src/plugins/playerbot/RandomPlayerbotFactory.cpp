#include "../pchdef.h"
#include "playerbot.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "../../server/database/Database/DatabaseEnv.h"
#include "PlayerbotAI.h"
#include "../../server/game/Entities/Player/Player.h"
#include "../../server/game/Guilds/Guild.h"
#include "../../server/game/Guilds/GuildMgr.h"
#include "../../server/game/DataStores/DB2Stores.h"
#include "../../server/game/Server/WorldSession.h"
#include "RandomPlayerbotFactory.h"

map<uint8, vector<uint8> > RandomPlayerbotFactory::availableRaces;

// Name pools are *data*, not code: the shipped seed in
// PlayerbotAIConfig::EnsureBotTables() plus sql/custom/playerbot/characters_playerbot.sql
// is what bots are named from. The previous version generated syllable names and
// INSERTed them on every CreateRandomBots() run (which also re-runs on the
// "retrying bot creation" path); with hundreds of accounts that filled the console
// with name-validation and "no names left" lines and wrote rows nobody asked for.
// Add names with INSERT INTO ai_playerbot_names (name) VALUES (...) instead - the
// picker below simply reports the pool as exhausted when it runs dry.

RandomPlayerbotFactory::RandomPlayerbotFactory(uint32 accountId) : accountId(accountId)
{
    if (!availableRaces.empty())
        return;

    availableRaces[CLASS_WARRIOR].push_back(RACE_HUMAN);
    availableRaces[CLASS_WARRIOR].push_back(RACE_NIGHTELF);
    availableRaces[CLASS_WARRIOR].push_back(RACE_GNOME);
    availableRaces[CLASS_WARRIOR].push_back(RACE_DWARF);
    availableRaces[CLASS_WARRIOR].push_back(RACE_ORC);
    availableRaces[CLASS_WARRIOR].push_back(RACE_UNDEAD_PLAYER);
    availableRaces[CLASS_WARRIOR].push_back(RACE_TAUREN);
    availableRaces[CLASS_WARRIOR].push_back(RACE_TROLL);
    availableRaces[CLASS_WARRIOR].push_back(RACE_DRAENEI);

    availableRaces[CLASS_PALADIN].push_back(RACE_HUMAN);
    availableRaces[CLASS_PALADIN].push_back(RACE_DWARF);
    availableRaces[CLASS_PALADIN].push_back(RACE_DRAENEI);
    availableRaces[CLASS_PALADIN].push_back(RACE_BLOODELF);

    availableRaces[CLASS_ROGUE].push_back(RACE_HUMAN);
    availableRaces[CLASS_ROGUE].push_back(RACE_DWARF);
    availableRaces[CLASS_ROGUE].push_back(RACE_NIGHTELF);
    availableRaces[CLASS_ROGUE].push_back(RACE_GNOME);
    availableRaces[CLASS_ROGUE].push_back(RACE_ORC);
    availableRaces[CLASS_ROGUE].push_back(RACE_TROLL);
    availableRaces[CLASS_ROGUE].push_back(RACE_BLOODELF);

    availableRaces[CLASS_PRIEST].push_back(RACE_HUMAN);
    availableRaces[CLASS_PRIEST].push_back(RACE_DWARF);
    availableRaces[CLASS_PRIEST].push_back(RACE_NIGHTELF);
    availableRaces[CLASS_PRIEST].push_back(RACE_DRAENEI);
    availableRaces[CLASS_PRIEST].push_back(RACE_TROLL);
    availableRaces[CLASS_PRIEST].push_back(RACE_UNDEAD_PLAYER);
    availableRaces[CLASS_PRIEST].push_back(RACE_BLOODELF);

    availableRaces[CLASS_MAGE].push_back(RACE_HUMAN);
    availableRaces[CLASS_MAGE].push_back(RACE_GNOME);
    availableRaces[CLASS_MAGE].push_back(RACE_DRAENEI);
    availableRaces[CLASS_MAGE].push_back(RACE_UNDEAD_PLAYER);
    availableRaces[CLASS_MAGE].push_back(RACE_TROLL);
    availableRaces[CLASS_MAGE].push_back(RACE_BLOODELF);

    availableRaces[CLASS_WARLOCK].push_back(RACE_HUMAN);
    availableRaces[CLASS_WARLOCK].push_back(RACE_GNOME);
    availableRaces[CLASS_WARLOCK].push_back(RACE_UNDEAD_PLAYER);
    availableRaces[CLASS_WARLOCK].push_back(RACE_ORC);
    availableRaces[CLASS_WARLOCK].push_back(RACE_BLOODELF);

    availableRaces[CLASS_SHAMAN].push_back(RACE_DRAENEI);
    availableRaces[CLASS_SHAMAN].push_back(RACE_ORC);
    availableRaces[CLASS_SHAMAN].push_back(RACE_TAUREN);
    availableRaces[CLASS_SHAMAN].push_back(RACE_TROLL);

    availableRaces[CLASS_HUNTER].push_back(RACE_DWARF);
    availableRaces[CLASS_HUNTER].push_back(RACE_NIGHTELF);
    availableRaces[CLASS_HUNTER].push_back(RACE_DRAENEI);
    availableRaces[CLASS_HUNTER].push_back(RACE_ORC);
    availableRaces[CLASS_HUNTER].push_back(RACE_TAUREN);
    availableRaces[CLASS_HUNTER].push_back(RACE_TROLL);
    availableRaces[CLASS_HUNTER].push_back(RACE_BLOODELF);

    availableRaces[CLASS_DRUID].push_back(RACE_NIGHTELF);
    availableRaces[CLASS_DRUID].push_back(RACE_TAUREN);
}

static void FillRandomCustomizations(WorldSession* session, uint8 race, uint8 cls, uint8 gender, WorldPackets::Array<UF::ChrCustomizationChoice, 250>& customizations)
{
    std::vector<ChrCustomizationOptionEntry const*> const* options = sDB2Manager.GetCustomiztionOptions(race, gender);
    if (!options)
        return;

    // Mirror the core's validated customization generation (see the .modify
    // gender command in cs_modify.cpp): both the option and every candidate
    // choice must pass MeetsChrCustomizationReq. The old code only filtered
    // choices and never checked the option's own requirement, so requirement
    // gated options (e.g. class/race locked) were picked and Player::Create
    // rejected the appearance with "invalid appearance attributes".
    Races raceId = Races(race);
    Classes classId = Classes(cls);
    std::vector<UF::ChrCustomizationChoice> selected;

    for (ChrCustomizationOptionEntry const* option : *options)
    {
        ChrCustomizationReqEntry const* optionReq = sChrCustomizationReqStore.LookupEntry(option->ChrCustomizationReqID);
        if (optionReq && !session->MeetsChrCustomizationReq(optionReq, raceId, classId, false, MakeChrCustomizationChoiceRange(selected)))
            continue;

        std::vector<ChrCustomizationChoiceEntry const*> const* choicesForOption = sDB2Manager.GetCustomiztionChoices(option->ID);
        if (!choicesForOption || choicesForOption->empty())
            continue;

        // gather every choice whose requirement passes with the options chosen
        // so far, then pick one at random
        std::vector<ChrCustomizationChoiceEntry const*> usable;
        for (ChrCustomizationChoiceEntry const* choice : *choicesForOption)
        {
            ChrCustomizationReqEntry const* choiceReq = sChrCustomizationReqStore.LookupEntry(choice->ChrCustomizationReqID);
            if (choiceReq && !session->MeetsChrCustomizationReq(choiceReq, raceId, classId, true, MakeChrCustomizationChoiceRange(selected)))
                continue;

            usable.push_back(choice);
        }

        if (usable.empty())
            continue;

        ChrCustomizationChoiceEntry const* picked = usable[urand(0, usable.size() - 1)];

        UF::ChrCustomizationChoice choice;
        choice.ChrCustomizationOptionID = option->ID;
        choice.ChrCustomizationChoiceID = picked->ID;
        selected.push_back(choice);
    }

    for (UF::ChrCustomizationChoice const& choice : selected)
        customizations.push_back(choice);
}

bool RandomPlayerbotFactory::CreateRandomBot(uint8 cls)
{
    TC_LOG_DEBUG("playerbot", "Creating new random bot for class {}", cls);

    // Reject unsupported explicit class requests before selecting a race.
    map<uint8, vector<uint8> >::const_iterator raceItr = availableRaces.find(cls);
    if (raceItr == availableRaces.end() || raceItr->second.empty())
    {
        TC_LOG_DEBUG("playerbot", "No race list for class {}, skipping random bot", cls);
        return false;
    }

    uint8 gender = rand() % 2 ? GENDER_MALE : GENDER_FEMALE;

    uint8 race = raceItr->second[urand(0, raceItr->second.size() - 1)];
    string name = CreateRandomBotName();
    if (name.empty())
        return false;

    std::string accountName;
    if (!AccountMgr::GetName(accountId, accountName))
        accountName = "rndbot";

    WorldSession* session = new WorldSession(accountId, std::move(accountName), 0, nullptr, SEC_PLAYER,
        uint8(sWorld->getIntConfig(CONFIG_EXPANSION)), 0, "", Minutes(0), LOCALE_enUS, 0, false);

    session->SetBotSession(true);

    Player* player = new Player(session);
    // Player::Create relies on the motion master being initialized exactly
    // like the core does it in HandleCharCreateOpcode.
    player->GetMotionMaster()->Initialize();

    WorldPackets::Character::CharacterCreateInfo cci;
    cci.Name = name;
    cci.Race = race;
    cci.Class = cls;
    cci.Sex = gender;
    FillRandomCustomizations(session, race, cls, gender, cci.Customizations);

    if (!player->Create(sObjectMgr->GetGenerator<HighGuid::Player>().Generate(), &cci))
    {
        // Counted and summarised by CreateRandomBots(): this runs once per class
        // per account, so a data problem here used to print one ERROR line per
        // bot (up to RandomBotAccountCount x 10) and bury everything else.
        TC_LOG_DEBUG("playerbot", "Unable to create random bot for account {} - name: \"{}\"; race: {}; class: {}",
                accountId, name.c_str(), race, cls);
        // the player is never added to a map; CleanupsBeforeDelete keeps the
        // Player destructor's ResetMap/grid accounting consistent
        player->CleanupsBeforeDelete();
        delete player;
        delete session;
        return false;
    }

    player->setCinematic(2);
    player->SetAtLoginFlag(AT_LOGIN_NONE);
    player->SaveToDB(true);

    // mirror core character creation: register the new character in the cache
    // so name/account/guild lookups by GUID (incl. bot login) can find it
    sCharacterCache->AddCharacterCacheEntry(player->GetGUID(), accountId, player->GetName(),
        player->GetNativeGender(), player->GetRace(), player->GetClass(), player->GetLevel(), false);

    TC_LOG_DEBUG("playerbot", "Random bot created for account {} - name: \"{}\"; race: {}; class: {}",
            accountId, name.c_str(), race, cls);

    // this player is never added to the world/map; run the same pre-delete
    // teardown the core uses before freeing it
    player->CleanupsBeforeDelete();
    delete player;
    delete session;
    return true;
}

string RandomPlayerbotFactory::CreateRandomBotName()
{
    QueryResult result = CharacterDatabase.Query("SELECT MAX(name_id), MIN(name_id) FROM ai_playerbot_names");
    if (!result || !result->GetRowCount())
    {
        // Deliberately quiet at this level: CreateRandomBot() is called once per
        // class per account, so an empty pool would print hundreds of identical
        // ERROR lines. CreateRandomBots() reports the whole pass once instead.
        TC_LOG_DEBUG("playerbot", "The ai_playerbot_names table is empty - no random bot names available");
        return "";
    }

    Field* fields = result->Fetch();
    uint32 maxId = fields[0].GetUInt32();
    uint32 minId = fields[1].GetUInt32();
    if (!maxId || maxId < minId)
    {
        TC_LOG_DEBUG("playerbot", "The ai_playerbot_names table is empty - no random bot names available");
        return "";
    }

    // A single random probe past the last free name used to report "no names
    // left" even when the table still had free entries (no ORDER BY and one
    // shot per call), which spammed an error for every class of every account
    // at each startup and silently left bot characters uncreated. Try several
    // random offsets, then fall back to the first free name in the table.
    for (int attempt = 0; attempt < 10; ++attempt)
    {
        uint32 id = urand(minId, maxId);
        result = CharacterDatabase.PQuery(
                "SELECT n.name FROM ai_playerbot_names n "
                "LEFT OUTER JOIN characters e ON e.name COLLATE utf8mb4_unicode_ci = n.name COLLATE utf8mb4_unicode_ci "
                "WHERE e.guid IS NULL AND n.name_id >= '{}' "
                "ORDER BY n.name_id LIMIT 1", id);
        if (result)
        {
            fields = result->Fetch();
            return fields[0].GetString();
        }
    }

    result = CharacterDatabase.PQuery(
            "SELECT n.name FROM ai_playerbot_names n "
            "LEFT OUTER JOIN characters e ON e.name COLLATE utf8mb4_unicode_ci = n.name COLLATE utf8mb4_unicode_ci "
            "WHERE e.guid IS NULL "
            "ORDER BY n.name_id LIMIT 1");
    if (result)
    {
        fields = result->Fetch();
        return fields[0].GetString();
    }

    TC_LOG_DEBUG("playerbot", "No more names left in ai_playerbot_names for random bots");
    return "";
}


static string RandomBotAccountPattern(string const& prefix)
{
    // Config validation excludes quotes and SQL wildcards except '_', which is
    // a legal account-name character. Escape it explicitly for LIKE; using '='
    // as escape character is independent of MySQL's NO_BACKSLASH_ESCAPES mode.
    string pattern;
    for (char c : prefix)
        pattern += c == '_' ? "=_" : string(1, c);
    return pattern + '%';
}

void RandomPlayerbotFactory::CreateRandomBots()
{
    // This runs at startup and can re-run later (creation retry, "rndbot init"):
    // rebuild the account list from scratch so repeated runs do not accumulate
    // duplicate entries (every extra entry costs character queries each tick).
    sPlayerbotAIConfig.randomBotAccounts.clear();

    string accountPattern = RandomBotAccountPattern(sPlayerbotAIConfig.randomBotAccountPrefix);
    if (sPlayerbotAIConfig.deleteRandomBotAccounts)
    {
        TC_LOG_INFO("playerbot",  "Deleting random bot accounts...");
        QueryResult results = LoginDatabase.PQuery("SELECT id FROM account where username LIKE '{}' ESCAPE '='", accountPattern);
        if (results)
        {
            do
            {
                Field* fields = results->Fetch();
                sAccountMgr->DeleteAccount(fields[0].GetUInt32());
            } while (results->NextRow());
        }

        CharacterDatabase.Execute("DELETE FROM ai_playerbot_random_bots");
        TC_LOG_INFO("playerbot",  "Random bot accounts deleted");
    }

    for (int accountNumber = 0; accountNumber < sPlayerbotAIConfig.randomBotAccountCount; ++accountNumber)
    {
        ostringstream out; out << sPlayerbotAIConfig.randomBotAccountPrefix << accountNumber;
        string accountName = out.str();
        QueryResult results = LoginDatabase.PQuery("SELECT id FROM account where username = '{}'", accountName.c_str());
        if (results)
        {
            continue;
        }

        string password = "";
        for (int i = 0; i < 10; i++)
        {
            password += (char)urand('!', 'z');
        }
        sAccountMgr->CreateAccount(accountName, password, "playerbot");

        TC_LOG_DEBUG("playerbot",  "Account {} created for random bots", accountName.c_str());
    }

    LoginDatabase.PExecute("UPDATE account SET expansion = '{}' where username LIKE '{}' ESCAPE '='", 2, accountPattern);

    int totalRandomBotChars = 0;
    // A single failure (one invalid name row, one rejected appearance) must not
    // abort creation for every account, but a genuinely exhausted name pool must
    // not spam failures either: retry each slot with a fresh random pick and stop
    // only after sustained consecutive failures. Existing characters on later
    // accounts are still registered either way.
    int const maxSlotAttempts = 3;
    int const maxConsecutiveFailures = 10;
    int consecutiveFailures = 0;
    int failedSlots = 0;
    for (int accountNumber = 0; accountNumber < sPlayerbotAIConfig.randomBotAccountCount; ++accountNumber)
    {
        ostringstream out; out << sPlayerbotAIConfig.randomBotAccountPrefix << accountNumber;
        string accountName = out.str();

        QueryResult results = LoginDatabase.PQuery("SELECT id FROM account where username = '{}'", accountName.c_str());
        if (!results)
            continue;

        Field* fields = results->Fetch();
        uint32 accountId = fields[0].GetUInt32();

        sPlayerbotAIConfig.randomBotAccounts.push_back(accountId);

        int count = sAccountMgr->GetCharactersCount(accountId);
        if (count >= 10 || consecutiveFailures >= maxConsecutiveFailures)
        {
            totalRandomBotChars += count;
            continue;
        }

        RandomPlayerbotFactory factory(accountId);

        for (uint8 cls = CLASS_WARRIOR; cls <= CLASS_DRUID && count < 10; ++cls)
        {
            if (cls == CLASS_MONK || cls == CLASS_DEATH_KNIGHT)
                continue;

            bool created = false;
            for (int attempt = 0; attempt < maxSlotAttempts && consecutiveFailures < maxConsecutiveFailures; ++attempt)
            {
                if (factory.CreateRandomBot(cls))
                {
                    created = true;
                    consecutiveFailures = 0;
                    break;
                }
                ++consecutiveFailures;
            }
            if (!created)
            {
                ++failedSlots;
                continue;
            }

            ++count;
        }

        totalRandomBotChars += sAccountMgr->GetCharactersCount(accountId);
    }

    // One line for the whole pass. The name pool is operator data now (the bot no
    // longer generates and INSERTs names for you), so "ran out of names" is a real
    // configuration answer and worth printing - just once, with the number of rows
    // it actually saw, instead of once per rejected character.
    if (failedSlots)
    {
        QueryResult pool = CharacterDatabase.Query("SELECT COUNT(*) FROM ai_playerbot_names");
        uint32 poolSize = pool ? pool->Fetch()[0].GetUInt32() : 0;
        TC_LOG_ERROR("playerbot", "{} random bot character slot(s) could not be created. The ai_playerbot_names "
            "pool holds {} row(s) and creation is limited to {} characters (AiPlayerbot.RandomBotAccountCount x 10, "
            "one name per character, names already taken by characters are skipped); add rows with "
            "INSERT INTO ai_playerbot_names (name) VALUES ('...') or lower the account count.",
            failedSlots, poolSize, sPlayerbotAIConfig.randomBotAccountCount * 10);
    }

    // Zero accounts or zero characters means no random bot can ever log in -
    // say so loudly instead of burying it in an INFO line: the usual causes are an
    // exhausted ai_playerbot_names pool (reported just above), rejected character
    // creation, or account creation failures in the login database.
    if (sPlayerbotAIConfig.randomBotAccounts.empty() || !totalRandomBotChars)
        TC_LOG_ERROR("playerbot", "Only {} random bot accounts with {} characters available - no random bots can appear in game. "
            "Check the errors above, the ai_playerbot_names name pool, and that AiPlayerbot.RandomBotAccountCount characters could be created.",
            sPlayerbotAIConfig.randomBotAccounts.size(), totalRandomBotChars);
    else
        TC_LOG_INFO("playerbot",  "{} random bot accounts with {} characters available", sPlayerbotAIConfig.randomBotAccounts.size(), totalRandomBotChars);
}


void RandomPlayerbotFactory::CreateRandomGuilds()
{
    vector<uint32> randomBots;
    QueryResult results = LoginDatabase.PQuery("SELECT id FROM account where username like '{}%'", sPlayerbotAIConfig.randomBotAccountPrefix.c_str());
    if (results)
    {
        do
        {
            Field* fields = results->Fetch();
            uint32 accountId = fields[0].GetUInt32();

            QueryResult results2 = CharacterDatabase.PQuery("SELECT guid FROM characters where account  = '{}'", accountId);
            if (results2)
            {
                do
                {
                    Field* fields = results2->Fetch();
                    uint32 guid = fields[0].GetUInt32();
                    randomBots.push_back(guid);
                } while (results2->NextRow());
            }

        } while (results->NextRow());
    }

    if (sPlayerbotAIConfig.deleteRandomBotGuilds)
    {
        TC_LOG_INFO("playerbot",  "Deleting random bot guilds...");
        for (vector<uint32>::iterator i = randomBots.begin(); i != randomBots.end(); ++i)
        {
            ObjectGuid leader = ObjectGuid::Create<HighGuid::Player>(*i);
            Guild* guild = sGuildMgr->GetGuildByLeader(leader);
            if (guild) guild->Disband();
        }
        TC_LOG_INFO("playerbot",  "Random bot guilds deleted");
    }

    int guildNumber = 0;
    vector<ObjectGuid> availableLeaders;
    for (vector<uint32>::iterator i = randomBots.begin(); i != randomBots.end(); ++i)
    {
        ObjectGuid leader = ObjectGuid::Create<HighGuid::Player>(*i);
        Guild* guild = sGuildMgr->GetGuildByLeader(leader);
        if (guild)
        {
            ++guildNumber;
            sPlayerbotAIConfig.randomBotGuilds.push_back(guild->GetId());
        }
        else
        {
            Player* player = ObjectAccessor::FindPlayer(leader);
            if (player)
                availableLeaders.push_back(leader);
        }
    }

    for (; guildNumber < sPlayerbotAIConfig.randomBotGuildCount; ++guildNumber)
    {
        string guildName = CreateRandomGuildName();
        if (guildName.empty())
            break;

        if (availableLeaders.empty())
        {
            TC_LOG_ERROR("playerbot",  "No leaders for random guilds available");
            break;
        }

        int index = urand(0, availableLeaders.size() - 1);
        ObjectGuid leader = availableLeaders[index];
        // a player may lead (and belong to) only one guild: remove the
        // chosen leader so it can't be picked again for the next guild
        availableLeaders.erase(availableLeaders.begin() + index);
        Player* player = ObjectAccessor::FindPlayer(leader);
        if (!player)
        {
            TC_LOG_ERROR("playerbot", "Cannot find player for leader {}", leader.ToString());
            break;
        }

        Guild* guild = new Guild();
        if (!guild->Create(player, guildName))
        {
            TC_LOG_ERROR("playerbot", "Error creating guild {}", guildName.c_str());
            delete guild;
            break;
        }

        sGuildMgr->AddGuild(guild);
        sPlayerbotAIConfig.randomBotGuilds.push_back(guild->GetId());
    }

    TC_LOG_INFO("playerbot",  "{} random bot guilds available", guildNumber);
}

string RandomPlayerbotFactory::CreateRandomGuildName()
{
    QueryResult result = CharacterDatabase.Query("SELECT MAX(name_id), MIN(name_id) FROM ai_playerbot_guild_names");
    if (!result || !result->GetRowCount())
    {
        TC_LOG_DEBUG("playerbot",  "No more names left in ai_playerbot_guild_names for random guilds");
        return "";
    }

    Field *fields = result->Fetch();
    uint32 maxId = fields[0].GetUInt32();
    uint32 minId = fields[1].GetUInt32();
    if (!maxId || maxId < minId)
    {
        TC_LOG_DEBUG("playerbot",  "No more names left in ai_playerbot_guild_names for random guilds");
        return "";
    }

    // Same single-probe flaw the bot-name picker used to have: one random
    // offset past the last free row reported "no names left" while free rows
    // remained. Retry several offsets, then take the first free row.
    for (int attempt = 0; attempt < 10; ++attempt)
    {
        uint32 id = urand(minId, maxId);
        result = CharacterDatabase.PQuery("SELECT n.name FROM ai_playerbot_guild_names n "
                "LEFT OUTER JOIN guild e ON e.name COLLATE utf8mb4_unicode_ci = n.name COLLATE utf8mb4_unicode_ci "
                "WHERE e.guildid IS NULL AND n.name_id >= '{}' "
                "ORDER BY n.name_id LIMIT 1", id);
        if (result)
        {
            fields = result->Fetch();
            return fields[0].GetString();
        }
    }

    result = CharacterDatabase.PQuery("SELECT n.name FROM ai_playerbot_guild_names n "
            "LEFT OUTER JOIN guild e ON e.name COLLATE utf8mb4_unicode_ci = n.name COLLATE utf8mb4_unicode_ci "
            "WHERE e.guildid IS NULL "
            "ORDER BY n.name_id LIMIT 1");
    if (result)
    {
        fields = result->Fetch();
        return fields[0].GetString();
    }

    TC_LOG_ERROR("playerbot",  "No more names left for random guilds");
    return "";
}

