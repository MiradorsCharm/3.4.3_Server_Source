-- Playerbot AI - characters database schema.
--
-- Apply once to your characters database:
--   mysql -u trinity -p characters < sql/custom/playerbot/characters_playerbot.sql
--
-- Every table carries an explicit COLLATE utf8mb4_unicode_ci on purpose: it is
-- the collation of the core tables these JOIN against (characters), and a
-- table left to the database default (utf8mb4_0900_ai_ci on MySQL 8) makes
-- every name lookup fail with errno 1267 "Illegal mix of collations".
--
-- The script is idempotent (CREATE TABLE IF NOT EXISTS) and can be re-run.
-- (It is also created automatically on worldserver startup if missing.)

-- Bot state. One row per bot character.
--
-- A bot is a real character, so its position, level, gear and spells already
-- live in the `characters` table and come back exactly as they were. What the
-- core does NOT know is that a character was *being a bot*: that it was
-- online, who owned it, and what it had been told to do. That is what this
-- table holds, and it is what makes the roster come back after a restart at
-- the same places with the same orders instead of a fresh crowd in Goldshire.
--
-- Written periodically (AiPlayerbot.StateSaveIntervalMs) and on every bot
-- logout / worldserver shutdown.
CREATE TABLE IF NOT EXISTS `characters_playerbot` (
    `guid` INT UNSIGNED NOT NULL,
    `master` INT UNSIGNED NOT NULL DEFAULT 0,
    `random_bot` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `tank_mode` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `grind_mode` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `stay` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `stay_x` FLOAT NOT NULL DEFAULT 0,
    `stay_y` FLOAT NOT NULL DEFAULT 0,
    `stay_z` FLOAT NOT NULL DEFAULT 0,
    `prepared_level` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `last_seen` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Playerbot state (roster, owner, orders)';

-- Upgrading from the previous build, whose table carried only (guid, master):
-- the worldserver adds the missing columns on startup (BotState::EnsureSchema),
-- so this only matters if you want the migration without starting the server.
-- MySQL has no "ADD COLUMN IF NOT EXISTS", hence the information_schema guard.
-- Re-running the whole file is safe: every statement below is guarded too.
SET @db := DATABASE();

SET @sql := IF((SELECT COUNT(*) FROM information_schema.COLUMNS
                WHERE TABLE_SCHEMA = @db AND TABLE_NAME = 'characters_playerbot' AND COLUMN_NAME = 'random_bot') = 0,
    'ALTER TABLE characters_playerbot ADD COLUMN random_bot TINYINT UNSIGNED NOT NULL DEFAULT 0', 'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @sql := IF((SELECT COUNT(*) FROM information_schema.COLUMNS
                WHERE TABLE_SCHEMA = @db AND TABLE_NAME = 'characters_playerbot' AND COLUMN_NAME = 'tank_mode') = 0,
    'ALTER TABLE characters_playerbot ADD COLUMN tank_mode TINYINT UNSIGNED NOT NULL DEFAULT 0', 'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @sql := IF((SELECT COUNT(*) FROM information_schema.COLUMNS
                WHERE TABLE_SCHEMA = @db AND TABLE_NAME = 'characters_playerbot' AND COLUMN_NAME = 'grind_mode') = 0,
    'ALTER TABLE characters_playerbot ADD COLUMN grind_mode TINYINT UNSIGNED NOT NULL DEFAULT 0', 'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @sql := IF((SELECT COUNT(*) FROM information_schema.COLUMNS
                WHERE TABLE_SCHEMA = @db AND TABLE_NAME = 'characters_playerbot' AND COLUMN_NAME = 'stay') = 0,
    'ALTER TABLE characters_playerbot ADD COLUMN stay TINYINT UNSIGNED NOT NULL DEFAULT 0', 'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @sql := IF((SELECT COUNT(*) FROM information_schema.COLUMNS
                WHERE TABLE_SCHEMA = @db AND TABLE_NAME = 'characters_playerbot' AND COLUMN_NAME = 'stay_x') = 0,
    'ALTER TABLE characters_playerbot ADD COLUMN stay_x FLOAT NOT NULL DEFAULT 0', 'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @sql := IF((SELECT COUNT(*) FROM information_schema.COLUMNS
                WHERE TABLE_SCHEMA = @db AND TABLE_NAME = 'characters_playerbot' AND COLUMN_NAME = 'stay_y') = 0,
    'ALTER TABLE characters_playerbot ADD COLUMN stay_y FLOAT NOT NULL DEFAULT 0', 'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @sql := IF((SELECT COUNT(*) FROM information_schema.COLUMNS
                WHERE TABLE_SCHEMA = @db AND TABLE_NAME = 'characters_playerbot' AND COLUMN_NAME = 'stay_z') = 0,
    'ALTER TABLE characters_playerbot ADD COLUMN stay_z FLOAT NOT NULL DEFAULT 0', 'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @sql := IF((SELECT COUNT(*) FROM information_schema.COLUMNS
                WHERE TABLE_SCHEMA = @db AND TABLE_NAME = 'characters_playerbot' AND COLUMN_NAME = 'prepared_level') = 0,
    'ALTER TABLE characters_playerbot ADD COLUMN prepared_level TINYINT UNSIGNED NOT NULL DEFAULT 0', 'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @sql := IF((SELECT COUNT(*) FROM information_schema.COLUMNS
                WHERE TABLE_SCHEMA = @db AND TABLE_NAME = 'characters_playerbot' AND COLUMN_NAME = 'last_seen') = 0,
    'ALTER TABLE characters_playerbot ADD COLUMN last_seen INT UNSIGNED NOT NULL DEFAULT 0', 'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- A master whose bots are gone should not keep re-adding dead characters:
-- rows for deleted characters are dropped on every run.
DELETE FROM characters_playerbot
 WHERE guid NOT IN (SELECT guid FROM characters);

-- Pool of names used when random bot characters are created. Operator data:
-- the server only reads free rows; add names with plain INSERTs.
--   INSERT INTO ai_playerbot_names (name) VALUES ('Nameone'), ('Nametwo');
-- A bot pass needs at least AiPlayerbot.RandomBotCount unused names.
CREATE TABLE IF NOT EXISTS `ai_playerbot_names` (
    `name_id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `name` VARCHAR(12) NOT NULL,
    `gender` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`name_id`),
    UNIQUE KEY `idx_name` (`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Playerbot random character names';

-- Notes for operators upgrading from the previous ike3/mangosbot port:
--  * your existing bot characters keep working: add them with `.bot add <name>`;
--  * random bots are every character whose ACCOUNT name starts with
--    AiPlayerbot.RandomBotAccountPrefix (default "rndbot") - old random bot
--    accounts created by the previous system are picked up automatically;
--  * the old ai_playerbot_random_bots / ai_playerbot_custom_strategy /
--    ai_playerbot_tellitem / ai_playerbot_guild_tasks / ai_playerbot_texts
--    tables are no longer read and can be dropped.
