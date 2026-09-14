-- ---------------------------------------------------------------------------
-- Fix for: "[1267] Illegal mix of collations (utf8mb4_unicode_ci,IMPLICIT) and
--           (utf8mb4_0900_ai_ci,IMPLICIT) for operation '='"
--
-- What happened: the ai_playerbot_* tables were created with only
-- DEFAULT CHARSET=utf8mb4 (no explicit collation), so they inherited the
-- DATABASE default collation. On MySQL 8 a database created without an
-- explicit collation defaults to utf8mb4_0900_ai_ci, while the core tables
-- they are JOINed against (characters.name, guild.name) are
-- utf8mb4_unicode_ci / utf8mb4_bin. MySQL refuses to compare columns with
-- different implicit collations, so every name lookup failed with errno 1267
-- and the server logged "No more names left for random guilds" while the name
-- pools were still full (and random bot characters could not be created).
--
-- Run this ONCE against the characters database:
--   mysql -u root -p characters < sql/custom/playerbot/fix_collations.sql
--
-- It is safe to re-run (idempotent), and the server also performs the same
-- repair automatically at startup (PlayerbotAIConfig::EnsureBotTable), so
-- running this file by hand is optional - it just fixes the database without
-- waiting for (or booting) the server.
--
-- NOTE: nothing here deletes or rewrites rows; CONVERT TO only changes the
-- collation/charset of the existing columns.
-- ---------------------------------------------------------------------------

-- 1. Future tables created in this database (without an explicit collation)
--    must default to the same collation as the core schema.
ALTER DATABASE `characters` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

-- 2. Convert the playerbot tables that were created with a foreign collation.
ALTER TABLE `ai_playerbot_random_bots`        CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
ALTER TABLE `ai_playerbot_names`              CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
ALTER TABLE `ai_playerbot_guild_names`        CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
ALTER TABLE `ai_playerbot_guild_tasks`        CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
ALTER TABLE `ai_playerbot_speech`             CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
ALTER TABLE `ai_playerbot_speech_probability` CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
ALTER TABLE `ai_playerbot_custom_strategy`    CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

-- 3. Verify: every row must be gone. (information_schema may lag one
--    transaction behind on older MySQL 8 versions - if a row survives,
--    re-run the file.)
SELECT TABLE_NAME, TABLE_COLLATION
FROM information_schema.TABLES
WHERE TABLE_SCHEMA = DATABASE()
  AND TABLE_NAME LIKE 'ai_playerbot%'
  AND TABLE_COLLATION <> 'utf8mb4_unicode_ci';
