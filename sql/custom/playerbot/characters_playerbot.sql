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

-- Bot -> master bindings. A row makes the bot log back in automatically the
-- next time its master enters the world (written by `.bot add`, cleared by
-- `.bot remove`; also created automatically on worldserver startup).
CREATE TABLE IF NOT EXISTS `characters_playerbot` (
    `guid` INT UNSIGNED NOT NULL,
    `master` INT UNSIGNED NOT NULL,
    PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Playerbot master bindings';

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
