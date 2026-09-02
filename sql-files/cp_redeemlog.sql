--
-- Table structure for table `cp_redeemlog`
--
-- Used by FluxCP's Flux_Athena::awardItem() (donation approvals) and by
-- the in-game "Donor Rewards Redeemer" NPC (data/npc/DonationNPC.txt),
-- which reads unclaimed rows (redeemed = 0) and hands out the item.
--
-- Not shipped by default in FluxCP's data/schemas — created manually
-- for FateRO on 2026-09-02.
--

CREATE TABLE IF NOT EXISTS `cp_redeemlog` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `nameid` int(11) NOT NULL DEFAULT '0',
  `quantity` int(11) NOT NULL DEFAULT '0',
  `cost` int(11) NOT NULL DEFAULT '0',
  `account_id` int(11) NOT NULL DEFAULT '0',
  `char_id` int(11) DEFAULT NULL,
  `redeemed` tinyint(1) NOT NULL DEFAULT '0',
  `redemption_date` datetime DEFAULT NULL,
  `purchase_date` datetime NOT NULL,
  `credits_before` int(10) NOT NULL DEFAULT '0',
  `credits_after` int(10) NOT NULL DEFAULT '0',
  PRIMARY KEY (`id`),
  KEY `account_id` (`account_id`),
  KEY `char_id` (`char_id`),
  KEY `redeemed` (`redeemed`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;