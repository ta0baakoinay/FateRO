--
-- FateShield (Gepard-style client protection) schema
--
-- These objects are also created automatically by the login-server on startup
-- when `fateshield_enabled: yes` in conf/fateshield.conf (see
-- src/login/account.cpp::account_fateshield_init and
-- src/login/loginlog.cpp::loginlog_fateshield_init).
--
-- This file is the reproducible, explicit form for deployment / CI. It is
-- idempotent and safe to re-run.
--
-- Import the `login` / `fateshield_*` statements into the MAIN login database
-- (inter_athena.conf: login_server_db). Import the `loginlog` ALTER into the
-- LOG database (inter_athena.conf: log_db_db) -- that is where loginlog_fateshield_init
-- runs. If both point at the same schema, importing the whole file once is fine.
--

-- --------------------------------------------------------
-- Per-account unique-id tracking on the login table
-- --------------------------------------------------------

-- last unique id seen for the account (set on "login ok")
-- MySQL >= 8.0 / MariaDB >= 10.5 support IF NOT EXISTS on ADD COLUMN;
-- for older engines, ignore the "duplicate column" error on re-run.
ALTER TABLE `login` ADD COLUMN `last_unique_id` INT(11) UNSIGNED NOT NULL DEFAULT '0';
ALTER TABLE `login` ADD COLUMN `blocked_unique_id` INT(11) UNSIGNED NOT NULL DEFAULT '0';

-- unique id captured for each row of the auth log
ALTER TABLE `loginlog` ADD COLUMN `unique_id` INT(11) UNSIGNED NOT NULL DEFAULT '0';

-- --------------------------------------------------------
-- Active blocks (checked at client init-ack time)
-- --------------------------------------------------------

CREATE TABLE IF NOT EXISTS `fateshield_block` (
  `unique_id` INT(11) UNSIGNED NOT NULL DEFAULT '0',
  `unban_time` DATETIME NOT NULL,
  `reason` VARCHAR(50) NOT NULL,
  UNIQUE KEY `unique_id` (`unique_id`)
) ENGINE=MyISAM DEFAULT CHARSET=latin1;

-- --------------------------------------------------------
-- Audit log of every block/unblock action
-- --------------------------------------------------------

CREATE TABLE IF NOT EXISTS `fateshield_block_log` (
  `id` INT(11) UNSIGNED NOT NULL AUTO_INCREMENT,
  `unique_id` INT(11) UNSIGNED NOT NULL DEFAULT '0',
  `block_time` DATETIME NOT NULL,
  `unban_time` DATETIME NOT NULL,
  `violator_name` VARCHAR(24) NOT NULL,
  `violator_account_id` INT(11) NOT NULL,
  `initiator_name` VARCHAR(24) NOT NULL,
  `initiator_account_id` INT(11) NOT NULL,
  `reason` VARCHAR(50) NOT NULL,
  PRIMARY KEY (`id`)
) ENGINE=MyISAM DEFAULT CHARSET=latin1 AUTO_INCREMENT=1;

-- --------------------------------------------------------
-- Client-submitted diagnostic / tamper reports
-- --------------------------------------------------------

CREATE TABLE IF NOT EXISTS `fateshield_report_log` (
  `time` DATETIME NOT NULL,
  `unique_id` INT(11) UNSIGNED NOT NULL DEFAULT '0',
  `account_id` INT(11) UNSIGNED NOT NULL DEFAULT '0',
  `char_id` INT(11) UNSIGNED NOT NULL DEFAULT '0',
  `char_name` VARCHAR(24) NOT NULL,
  `report_str` VARCHAR(120) NOT NULL
) ENGINE=MyISAM DEFAULT CHARSET=latin1;
