-- ---------------------------------------------------------------------------
-- 2026-09-07  Performance indexes
--
-- Adds the missing `nameid` indexes on the item-holding tables. Stock rAthena
-- ships these tables with only a char_id / account_id / guild_id key, so any
-- lookup "how many / who has item N" (the Searcher NPC, web panels, GM tools)
-- is a full table scan that gets linearly slower as the tables grow over
-- weeks of uptime. These indexes make those queries index range-scans instead.
--
-- Also indexes acc_reg_num(`key`) so the daily MusicBox cleanup
-- (DELETE FROM acc_reg_num WHERE `key` = '#mb...') stops full-scanning; the
-- table's PRIMARY KEY is (account_id, `key`) which a WHERE on `key` alone
-- cannot use.
--
-- Purely additive: no rows change, no application behaviour changes, only
-- read plans improve and each write pays one extra small index update.
-- Safe to run on a live server (ALGORITHM=INPLACE on MariaDB/InnoDB).
-- Requires MariaDB 10.5+ / MySQL 8.0+ for "ADD INDEX IF NOT EXISTS".
-- ---------------------------------------------------------------------------

ALTER TABLE `inventory`        ADD INDEX IF NOT EXISTS `nameid` (`nameid`);
ALTER TABLE `cart_inventory`   ADD INDEX IF NOT EXISTS `nameid` (`nameid`);
ALTER TABLE `storage`          ADD INDEX IF NOT EXISTS `nameid` (`nameid`);
ALTER TABLE `guild_storage`    ADD INDEX IF NOT EXISTS `nameid` (`nameid`);
ALTER TABLE `mail_attachments` ADD INDEX IF NOT EXISTS `nameid` (`nameid`);

ALTER TABLE `acc_reg_num`      ADD INDEX IF NOT EXISTS `key` (`key`);

-- ---------------------------------------------------------------------------
-- Population Engine: cp_population_stats
--
-- Canonical schema also lives in sql-files/population_engine.sql (an optional
-- import that is easy to miss). The map-server now also CREATEs it on start
-- (src/map/population_engine.cpp::do_init_population_engine), so this block is
-- only needed for a DB that hit "Table '<db>.cp_population_stats' doesn't
-- exist" before that build was deployed. Keep in sync with
-- sql-files/population_engine.sql.
-- ---------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS `cp_population_stats` (
  `id`           INT UNSIGNED NOT NULL DEFAULT 1,
  `active_count` INT UNSIGNED NOT NULL DEFAULT 0,
  `last_updated` TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT IGNORE INTO `cp_population_stats` (`id`, `active_count`) VALUES (1, 0);

-- Note: the Searcher card-slot lookups (WHERE card0=N OR card1=N OR card2=N
-- OR card3=N) still cannot use a plain index. They are admin/rare; if they
-- become hot, add a summary table or per-slot indexes with index_merge.
-- Not indexing char(`zeny`): it changes on nearly every transaction, so the
-- write cost outweighs the one rare "richest players" query.
