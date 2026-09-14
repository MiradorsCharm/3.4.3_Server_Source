--
-- AI Playerbot (ported from ike3/mangosbot) - characters database schema
--
-- Apply this file to your characters database:
--   mysql -u trinity -p characters < sql/custom/playerbot/characters_playerbot.sql
--
-- Every table carries an explicit COLLATE utf8mb4_unicode_ci on purpose: it is
-- the collation of the core tables these JOIN against (characters, guild), and
-- a table left to the database default (utf8mb4_0900_ai_ci on MySQL 8) makes
-- every name lookup fail with errno 1267 "Illegal mix of collations".
--

-- Random bot bookkeeping (login/logout/randomize/teleport schedules).
CREATE TABLE IF NOT EXISTS `ai_playerbot_random_bots` (
    `owner` INT UNSIGNED NOT NULL DEFAULT 0,
    `bot` INT UNSIGNED NOT NULL DEFAULT 0,
    `time` INT UNSIGNED NOT NULL DEFAULT 0,
    `validIn` INT UNSIGNED NOT NULL DEFAULT 0,
    `event` VARCHAR(64) NOT NULL DEFAULT '',
    `value` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`owner`, `bot`, `event`),
    KEY `idx_event` (`event`),
    KEY `idx_bot` (`bot`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Playerbot random bot events';

-- Pool of names used when random bot characters are created.
CREATE TABLE IF NOT EXISTS `ai_playerbot_names` (
    `name_id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `name` VARCHAR(12) NOT NULL,
    `gender` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`name_id`),
    UNIQUE KEY `idx_name` (`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Playerbot random character names';

-- Pool of names used when random bot guilds are created.
CREATE TABLE IF NOT EXISTS `ai_playerbot_guild_names` (
    `name_id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `name` VARCHAR(24) NOT NULL,
    PRIMARY KEY (`name_id`),
    UNIQUE KEY `idx_name` (`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Playerbot random guild names';

-- Guild tasks handed out by bot guilds.
CREATE TABLE IF NOT EXISTS `ai_playerbot_guild_tasks` (
    `owner` INT UNSIGNED NOT NULL DEFAULT 0,
    `guildid` INT UNSIGNED NOT NULL DEFAULT 0,
    `time` INT UNSIGNED NOT NULL DEFAULT 0,
    `validIn` INT UNSIGNED NOT NULL DEFAULT 0,
    `type` VARCHAR(32) NOT NULL DEFAULT '',
    `value` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`owner`, `guildid`, `type`),
    KEY `idx_guild` (`guildid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Playerbot guild tasks';

-- Free form chatter used by the "chat" strategy.
CREATE TABLE IF NOT EXISTS `ai_playerbot_speech` (
    `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `name` VARCHAR(64) NOT NULL,
    `text` VARCHAR(255) NOT NULL,
    `type` VARCHAR(16) NOT NULL DEFAULT 'say',
    PRIMARY KEY (`id`),
    KEY `idx_name` (`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Playerbot chatter';

CREATE TABLE IF NOT EXISTS `ai_playerbot_speech_probability` (
    `name` VARCHAR(64) NOT NULL,
    `probability` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Playerbot chatter probability';

-- User defined strategies (".bot strategy" / "co +custom::<name>").
CREATE TABLE IF NOT EXISTS `ai_playerbot_custom_strategy` (
    `name` VARCHAR(64) NOT NULL,
    `action_line` VARCHAR(255) NOT NULL,
    PRIMARY KEY (`name`, `action_line`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Playerbot custom strategies';

-- A handful of starter names so random bots can be created out of the box.
INSERT IGNORE INTO `ai_playerbot_names` (`name`) VALUES
('Aeltar'), ('Baldrin'), ('Cathmor'), ('Dornan'), ('Eldrik'), ('Faelan'),
('Gorvin'), ('Halbrik'), ('Ithran'), ('Jorlan'), ('Kelvar'), ('Lorwyn'),
('Mordak'), ('Nyrelle'), ('Orwin'), ('Perrin'), ('Quenna'), ('Rhogar'),
('Sylvara'), ('Torvald'), ('Ulther'), ('Varlen'), ('Wyndel'), ('Xanthe'),
('Yorik'), ('Zaltar'), ('Ashwyn'), ('Brannoc'), ('Cirien'), ('Draveth'),
('Elowen'), ('Fenwick'), ('Gwynor'), ('Harlow'), ('Isolde'), ('Jareth'),
('Kaelith'), ('Lyanna'), ('Merrick'), ('Norwyn'), ('Ondrel'), ('Pellan'),
('Rowena'), ('Selwyn'), ('Thalric'), ('Ulmara'), ('Verrik'), ('Wilrun'),
('Yalira'), ('Zeryth');

INSERT IGNORE INTO `ai_playerbot_guild_names` (`name`) VALUES
('The Wandering Blades'), ('Sons of Lordaeron'), ('Emerald Vanguard'),
('Ashen Company'), ('Stormwatch'), ('The Silver Hand Irregulars'),
('Dawnbreakers'), ('Ironforge Regulars'), ('Nightfall Covenant'),
('The Last Caravan');
