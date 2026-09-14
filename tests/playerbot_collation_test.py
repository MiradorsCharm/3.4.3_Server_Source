"""Guards against the errno 1267 "Illegal mix of collations" breakage.

The random-bot name pickers JOIN the plugin tables against the core tables:

    SELECT n.name FROM ai_playerbot_names n
    LEFT OUTER JOIN characters e ON e.name = n.name ...
    SELECT n.name FROM ai_playerbot_guild_names n
    LEFT OUTER JOIN guild e ON e.name = n.name ...

The core columns are utf8mb4_unicode_ci (guild.name) / utf8mb4_bin
(characters.name); the plugin tables were created with only
DEFAULT CHARSET=utf8mb4, so they inherited the *database* default collation -
utf8mb4_0900_ai_ci on any MySQL 8 database created without an explicit one,
utf8mb4_general_ci on the ones Setup-Database.ps1 used to build. MySQL refuses
to compare two columns whose implicit collations differ, so every lookup
failed with errno 1267 and the server reported "No more names left for random
guilds" (and could not name random bot characters) while the pools were full.

Three layers are pinned here, and all three must survive:
1. the JOIN predicates carry an explicit COLLATE, so they are legal no matter
   what collation the tables happen to have right now;
2. every CREATE TABLE (runtime auto-create and the shipped .sql) pins
   COLLATE=utf8mb4_unicode_ci, matching the core base schema;
3. the runtime table check (EnsureBotTable) converts a table that exists with
   a foreign collation, so a broken install heals itself on the next boot
   (sql/custom/playerbot/fix_collations.sql is the by-hand equivalent).

Run: python -m unittest discover -s tests -p "playerbot_*_test.py" -v
"""

import unittest

from playerbot_logic_test import ROOT


PLUGIN = "src/plugins/playerbot"
FACTORY = f"{PLUGIN}/RandomPlayerbotFactory.cpp"
CONFIG = f"{PLUGIN}/PlayerbotAIConfig.cpp"
SQL = "sql/custom/playerbot/characters_playerbot.sql"
FIX_SQL = "sql/custom/playerbot/fix_collations.sql"
SETUP = "Setup-Database.ps1"


def read(path):
    return (ROOT / path).read_text(encoding="utf-8", errors="replace")


class CollationProofQueryTest(unittest.TestCase):
    def test_name_picker_joins_pin_the_collation(self):
        """Both sides of every name comparison must be explicit: an explicit
        collation is coercibility-EXPLICIT, which never raises 1267, whatever
        the two tables carry."""
        text = read(FACTORY)
        for join in ("JOIN characters e", "JOIN guild e"):
            self.assertIn(join, text)
        # two character-name joins + two guild-name joins, an explicit collation
        # on BOTH sides of every comparison
        self.assertEqual(text.count("COLLATE utf8mb4_unicode_ci"), 8,
                         "every e.name = n.name comparison needs an explicit collation on both sides")
        for query in ("LEFT OUTER JOIN characters e ON", "LEFT OUTER JOIN guild e ON"):
            for line in text.splitlines():
                if query in line:
                    self.assertIn("COLLATE utf8mb4_unicode_ci", line,
                                  f"unpinned join predicate: {line.strip()}")


class CollationProofSchemaTest(unittest.TestCase):
    def test_runtime_create_tables_pin_the_collation(self):
        """The server auto-creates these tables on boot; without an explicit
        collation they inherit the database default and drift away from the
        core tables they are JOINed against."""
        text = read(CONFIG)
        self.assertEqual(text.count("COLLATE=utf8mb4_unicode_ci"), 7,
                         "all seven ai_playerbot_* CREATE TABLE statements must pin "
                         "utf8mb4_unicode_ci")
        self.assertNotIn("CHARSET=utf8mb4\"", text,
                         "a CREATE TABLE without COLLATE inherits the database default")

    def test_shipped_sql_pins_the_collation(self):
        text = read(SQL)
        self.assertEqual(text.count("COLLATE=utf8mb4_unicode_ci"), 7)

    def test_runtime_self_heal_converts_foreign_collations(self):
        body = read(CONFIG)
        self.assertIn("TABLE_COLLATION", body,
                      "EnsureBotTable must notice a table created with a foreign collation")
        self.assertIn("CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci", body,
                      "EnsureBotTable must repair the collation, not just log it")

    def test_setup_script_creates_databases_with_the_schema_collation(self):
        text = read(SETUP)
        self.assertNotIn("utf8mb4_general_ci", text,
                         "the databases must be created with the collation of the base "
                         "schema (utf8mb4_unicode_ci), not MySQL 5's general_ci")
        self.assertIn("COLLATE utf8mb4_unicode_ci", text)


class FixCollationsSqlTest(unittest.TestCase):
    def test_hand_fix_exists_and_is_safe(self):
        """The by-hand fix the operator is told to run."""
        text = read(FIX_SQL)
        for table in ("ai_playerbot_random_bots", "ai_playerbot_names",
                      "ai_playerbot_guild_names", "ai_playerbot_guild_tasks",
                      "ai_playerbot_speech", "ai_playerbot_speech_probability",
                      "ai_playerbot_custom_strategy"):
            self.assertIn(f"`{table}`", text)
        self.assertEqual(text.count("CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"), 7)
        self.assertIn("ALTER DATABASE `characters`", text)
        # nothing destructive: CONVERT TO changes collations only
        for banned in ("DROP TABLE", "TRUNCATE", "DELETE FROM"):
            self.assertNotIn(banned, text)


if __name__ == "__main__":
    unittest.main()
