-- =============================================================================================
-- mod-discord-chat - characters database migration
-- Persistent Discord admin audit log and persistent server-status message id.
-- =============================================================================================

DROP TABLE IF EXISTS `mod_discord_audit_log`;
CREATE TABLE `mod_discord_audit_log` (
    `id`             BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `discord_username` VARCHAR(64)   NOT NULL DEFAULT '' COMMENT 'Discord username at time of action',
    `discord_id`     VARCHAR(32)     NOT NULL DEFAULT '' COMMENT 'Discord user snowflake',
    `account_id`     INT UNSIGNED    NOT NULL DEFAULT 0 COMMENT 'Linked AzerothCore account, if any',
    `command`        VARCHAR(64)     NOT NULL COMMENT 'Command name, e.g. server.shutdown',
    `arguments`      TEXT            NULL COMMENT 'Serialized command arguments',
    `result`         VARCHAR(255)    NOT NULL DEFAULT '' COMMENT 'Result / status of the action',
    `realm_id`       INT UNSIGNED    NOT NULL DEFAULT 0 COMMENT 'Realm id (Discord.GameRealmId)',
    `created_at`     DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    KEY `idx_discord_id` (`discord_id`),
    KEY `idx_command` (`command`),
    KEY `idx_created_at` (`created_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Discord administrative actions (mod-discord-chat)';

DROP TABLE IF EXISTS `mod_discord_status`;
CREATE TABLE `mod_discord_status` (
    `realm_id`       INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Realm id (Discord.GameRealmId)',
    `channel_id`     VARCHAR(32)  NOT NULL DEFAULT '' COMMENT 'Persistent status channel',
    `message_id`     VARCHAR(32)  NOT NULL DEFAULT '' COMMENT 'Persistent status message id',
    `updated_at`     DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`realm_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Persistent Discord server-status message (mod-discord-chat)';