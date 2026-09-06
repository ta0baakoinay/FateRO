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

-- Note: the Searcher card-slot lookups (WHERE card0=N OR card1=N OR card2=N
-- OR card3=N) still cannot use a plain index. They are admin/rare; if they
-- become hot, add a summary table or per-slot indexes with index_merge.
-- Not indexing char(`zeny`): it changes on nearly every transaction, so the
-- write cost outweighs the one rare "richest players" query.
