"""Regression checks for the randomization safety audit (no DB/client data).

Production functions/blocks are compiled verbatim against API-contract fakes.
Use the same toolchain as playerbot_logic_test.py. These are focused tests, not
an alternative to the Windows worldserver build or a live-map integration test.
"""

from pathlib import Path
import re
import unittest

from playerbot_logic_test import ROOT, run_cpp, source_slice


FACTORY = "src/plugins/playerbot/PlayerbotFactory.cpp"
MANAGER = "src/plugins/playerbot/RandomPlayerbotMgr.cpp"
CONFIG = "src/plugins/playerbot/PlayerbotAIConfig.cpp"


def function(path, signature):
    """Extract one whole function (the selected functions have balanced braces)."""
    text = (ROOT / path).read_text(encoding="utf-8")
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def harness(name, blocks):
    text = Path(__file__).with_name(name).read_text(encoding="utf-8")
    for key, code in blocks.items():
        text = text.replace(f"@{key}@", code)
    assert not re.search(r"@[A-Z_]+@", text), "Unfilled harness block"
    return text


class PlayerbotSafetyTest(unittest.TestCase):
    def test_factory_pet_spells_combat_and_level_safety(self):
        blocks = {
            key: function(FACTORY, signature) for key, signature in (
                ("PET", "void PlayerbotFactory::InitPet()"),
                ("CLEAR_SPELLS", "void PlayerbotFactory::ClearSpells()"),
                ("PREPARE", "void PlayerbotFactory::Prepare()"),
                ("TALENTS", "void PlayerbotFactory::InitTalents()"),
                ("EQUIP", "bool PlayerbotFactory::CanEquipItem("),
                ("SKILLS", "void PlayerbotFactory::InitSkills()"),
                ("RANDOM_SKILL", "void PlayerbotFactory::SetRandomSkill("),
            )
        }
        blocks["MAX_LEVEL"] = source_slice(
            "src/server/game/World/World.cpp", "    if (m_int_configs[CONFIG_MAX_PLAYER_LEVEL] < 1", "    m_int_configs[CONFIG_MIN_DUALSPEC_LEVEL]")
        blocks["REFRESH"] = function(MANAGER, "void RandomPlayerbotMgr::Refresh(")
        blocks["TELEPORT"] = function(MANAGER, "void RandomPlayerbotMgr::RandomTeleport(Player* bot, vector<WorldLocation>")
        blocks["TELEPORT_LEVEL_GUARD"] = source_slice(
            MANAGER, "void RandomPlayerbotMgr::RandomTeleportForLevel(Player* bot)",
            # per-bot "Preparing location..." is debug-level now (see the console-noise
            # note in playerbot_combat_test.py); the slice just needs its end marker
            '    TC_LOG_DEBUG("playerbot",  "Preparing location')
        blocks["RESTORE_LEVEL"] = source_slice(FACTORY, "    // quest rewards boost bot level", "    ClearInventory();")
        blocks["BUDGET"] = source_slice(
            MANAGER, "    uint32 randomBotsPerInterval =", "    // processTicks was initialised")
        blocks["CONSOLE_REFRESH"] = source_slice(
            MANAGER, "                // Refresh at the current level", "            }\n            uint32 randomTime")
        run_cpp(self, harness("playerbot_factory_safety.cpp.in", blocks))

    def test_config_pricing_and_dependency_safety(self):
        blocks = {
            "LISTS": source_slice(CONFIG, "template <class T>\nvoid LoadList", "uint32 PlayerbotAIConfig::GetRandomChangeRange"),
            "CONFIG_CLASS": source_slice("src/plugins/playerbot/PlayerbotAIConfig.h", "class PlayerbotAIConfig", "#define sPlayerbotAIConfig"),
            "CONFIG_READS": source_slice(CONFIG, "    // Negative unsigned settings", "    RandomPlayerbotFactory::CreateRandomBots();"),
            "RANDOM_RANGE": function(CONFIG, "uint32 PlayerbotAIConfig::GetRandomChangeRange"),
            "AHBOT_CONFIG_CLASS": source_slice("src/plugins/ahbot/AhBotConfig.h", "class AhBotConfig", "#define sAhBotConfig"),
            "AHBOT_CONFIG": function("src/plugins/ahbot/AhBotConfig.cpp", "bool AhBotConfig::Initialize()"),
            "AHBOT_CLASS": source_slice("src/plugins/ahbot/AhBot.h", "namespace ahbot", "#define auctionbot"),
            "PRICING": (ROOT / "src/plugins/ahbot/AhBot.cpp").read_text().split("namespace\n", 1)[1],
            "RANDOM_ITEMS": function("src/plugins/playerbot/RandomItemMgr.cpp", "RandomItemList RandomItemMgr::Query(RandomItemType type)"),
            "STOCK": source_slice(FACTORY, "    uint32 count = 1, stacks = 1;", "void PlayerbotFactory::InitInventoryEquip()"),
            "ACCOUNT_PATTERN": function("src/plugins/playerbot/RandomPlayerbotFactory.cpp", "static string RandomBotAccountPattern"),
            "RACE_SETUP": function("src/plugins/playerbot/RandomPlayerbotFactory.cpp", "RandomPlayerbotFactory::RandomPlayerbotFactory("),
            "CLASS_LOOP": (
                # The retry budget is declared outside the class loop; extract it verbatim
                # too so the harness compiles the real tuning, not a stale copy.
                source_slice("src/plugins/playerbot/RandomPlayerbotFactory.cpp", "    int const maxSlotAttempts = 3;", "    for (int accountNumber = 0;")
                + function("src/plugins/playerbot/RandomPlayerbotFactory.cpp", "        for (uint8 cls = CLASS_WARRIOR;")),
            "SPELL_DEPENDENCIES": function("src/server/game/Spells/SpellMgr.cpp", "static bool WouldCreateSpellRequirementCycle"),
            "RANDOM_TRIGGER": function("src/plugins/playerbot/strategy/triggers/GenericTriggers.cpp", "bool RandomTrigger::IsActive()"),
        }
        shared = (ROOT / "src/server/game/Miscellaneous/SharedDefines.h").read_text()
        blocks["CLASSES"] = re.search(r"enum Classes : uint8\s*\{.*?\};", shared, re.DOTALL).group()
        races = (ROOT / "src/server/game/Miscellaneous/RaceMask.h").read_text()
        blocks["RACES"] = re.search(r"enum Races\s*\{.*?\};", races, re.DOTALL).group()
        # Keep the anonymous namespace around the pricing saturation helper.
        blocks["PRICING"] = "namespace\n" + blocks["PRICING"]
        run_cpp(self, harness("playerbot_config_safety.cpp.in", blocks))

    def test_guards_are_wired_into_live_paths(self):
        required = function("src/server/game/Spells/SpellMgr.cpp", "void SpellMgr::LoadSpellRequired()")
        self.assertLess(required.index("WouldCreateSpellRequirementCycle(mSpellReq"), required.index("mSpellReq.insert"))
        process = function(MANAGER, "bool RandomPlayerbotMgr::ProcessBot(uint32 bot)")
        self.assertLess(process.index("!player->IsInWorld() || player->IsBeingTeleported()"), process.index("Randomize(player)"))
        lfg = function("src/plugins/playerbot/strategy/actions/LfgActions.cpp", "bool LfgAcceptAction::Execute")
        self.assertIn("sPlayerbotAIConfig.GetRandomChangeRange(10.0)", lfg)
        self.assertNotIn("/ sPlayerbotAIConfig.randomChangeMultiplier", lfg)
        tick = function(MANAGER, "void RandomPlayerbotMgr::UpdateAIInternal(")
        self.assertLess(tick.index("botProcessed >= randomBotsPerInterval"), tick.index("ProcessBot(bot)"))
        initialize = function("src/plugins/playerbot/PlayerbotHookImpl.cpp", "    void InitializePlayerbots()")
        self.assertLess(initialize.index("if (!sPlayerbotAIConfig.Initialize())"), initialize.index("SetEnabled(true)"))
        self.assertLess(initialize.index("return;"), initialize.index("SetEnabled(true)"))
        self.assertIn("commandServerPort", initialize)
        ctor = function(MANAGER, "RandomPlayerbotMgr::RandomPlayerbotMgr()")
        self.assertNotIn("sPlayerbotCommandServer.Start()", ctor)
        load = function("src/server/worldserver/Main.cpp", "static void LoadAllScripts()")
        self.assertIn('GetBoolDefault("AiPlayerbot.Enabled"', load)
        self.assertLess(load.index('GetBoolDefault("AiPlayerbot.Enabled"'), load.index("RegisterPlayerbotScripts"))
        config_init = function(CONFIG, "bool PlayerbotAIConfig::Initialize()")
        self.assertLess(config_init.index('GetBoolDefault("AiPlayerbot.Enabled"'), config_init.index("EnsureBotTables"))
        self.assertLess(config_init.index("if (!enabled)"), config_init.index("CreateRandomBots"))
        trade = function(FACTORY, "void PlayerbotFactory::InitTradeSkills()")
        self.assertIn("HasSkill(tradeSkills[i])", trade)
        teleport = function(MANAGER, "void RandomPlayerbotMgr::RandomTeleport(Player* bot, vector<WorldLocation>")
        self.assertNotIn("HasBotTeleportNavigation", teleport)
        self.assertNotIn("GetNavMeshQuery", teleport)
        self.assertIn("no candidate passed terrain/area checks", teleport)
        self.assertNotIn("MMAP checks", teleport)
        # Empty grind-location / game_tele / factory item lists are expected
        # skips, not console ERROR (Logger.root=5 would print them).
        self.assertIn("no locations available", teleport)
        self.assertNotIn('TC_LOG_ERROR("playerbot",  "Cannot teleport bot {} - no locations available"', teleport)
        first = function(MANAGER, "void RandomPlayerbotMgr::RandomizeFirst(")
        self.assertIn("No game_tele locations", first)
        self.assertNotIn('TC_LOG_ERROR("playerbot", "No game_tele locations', first)
        self.assertIn('TC_LOG_ERROR("playerbot", "Cannot randomize bot {} - AiPlayerbot.RandomBotMaps is empty"', first)
        for signature, needle in (
            ("void PlayerbotFactory::InitPet()", "No pets available"),
            ("void PlayerbotFactory::InitPet()", "Cannot create pet"),
            ("void PlayerbotFactory::InitBags()", "no bags found"),
            ("void PlayerbotFactory::InitInventoryTrade()", "No trade items available"),
            ("void PlayerbotFactory::InitGlyphs()", "No glyphs found"),
            ("void PlayerbotFactory::InitTalents(uint32 specNo)", "No spells for talent row"),
            ("void PlayerbotFactory::InitGuild()", "No random guilds available"),
        ):
            body = function(FACTORY, signature)
            self.assertIn(needle, body)
            self.assertNotIn(f'TC_LOG_ERROR("playerbot"', body[body.index(needle) - 80:body.index(needle)])
        gtask_item = function("src/plugins/playerbot/GuildTaskMgr.cpp", "bool GuildTaskMgr::CreateItemTask(")
        self.assertIn("no items avaible for item task", gtask_item)
        self.assertNotIn('TC_LOG_ERROR("gtask"', gtask_item)
        gtask_kill = function("src/plugins/playerbot/GuildTaskMgr.cpp", "bool GuildTaskMgr::CreateKillTask(")
        self.assertIn("no rare creatures available", gtask_kill)
        self.assertNotIn('TC_LOG_ERROR("gtask"', gtask_kill)
        rnditem = function("src/plugins/playerbot/RandomItemMgr.cpp", "RandomItemList RandomItemMgr::Query(RandomItemType type)")
        self.assertIn("no items available for random item query", rnditem)
        self.assertNotIn('TC_LOG_ERROR("gtask",  "no items available for random item query"', rnditem)


if __name__ == "__main__":
    unittest.main()
