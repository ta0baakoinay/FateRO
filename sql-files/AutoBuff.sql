CREATE TABLE ab_common_config (
    `char_id` int UNSIGNED PRIMARY KEY,
    `following_player` INT UNSIGNED NOT NULL DEFAULT '0',
    `dist_to_leader` SMALLINT UNSIGNED NOT NULL DEFAULT '2',
    `autobuff_resurection` BOOLEAN NOT NULL DEFAULT '1',
    `skill_cd` BIGINT NOT NULL DEFAULT '0',
    `allow_pm_cmd` SMALLINT UNSIGNED NOT NULL DEFAULT '0',
    `autobuff_potions` BOOLEAN NOT NULL DEFAULT '1',
    `return_to_savepoint` BOOLEAN NOT NULL DEFAULT '0',
    `autobuff_token_siegfried` BOOLEAN NOT NULL DEFAULT '0',
    `autobuff_disable_alone` BOOLEAN NOT NULL DEFAULT '1',
    `priorize_buff` BOOLEAN NOT NULL DEFAULT '0'
);

CREATE TABLE ab_skills (
    `ab_skills_id` BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    `char_id` int UNSIGNED NOT NULL,
	`type` smallint UNSIGNED NOT NULL,
    `skill_id` SMALLINT UNSIGNED NOT NULL,
    `skill_lv` SMALLINT UNSIGNED NOT NULL DEFAULT '0',
    `min_hp` SMALLINT UNSIGNED NOT NULL DEFAULT '0',
    `last_use` BIGINT NOT NULL DEFAULT '0',
    `priorize_type` SMALLINT UNSIGNED NOT NULL,
    FOREIGN KEY (char_id) REFERENCES ab_common_config(char_id)
);
ALTER TABLE `ab_skills` ADD UNIQUE KEY `char_id` (`char_id`,`type`,`skill_id`);


CREATE TABLE ab_skills_char_ids (
    `ab_skills_id` BIGINT UNSIGNED NOT NULL,
    `ab_char_id` INT UNSIGNED NOT NULL,
    FOREIGN KEY (ab_skills_id) REFERENCES ab_skills(ab_skills_id)
);
ALTER TABLE `ab_skills_char_ids` ADD UNIQUE KEY `chars_id` (`ab_skills_id`,`ab_char_id`);

CREATE TABLE `ab_items` (
  `char_id` int UNSIGNED NOT NULL,
  `type` smallint UNSIGNED NOT NULL,
  `item_id` int UNSIGNED NOT NULL,
  `min_hp` smallint UNSIGNED NOT NULL DEFAULT '0',
  `min_sp` smallint UNSIGNED NOT NULL DEFAULT '0',
  `status` int UNSIGNED NOT NULL DEFAULT '0'
);
ALTER TABLE `ab_items` ADD UNIQUE KEY `char_id` (`char_id`,`type`,`item_id`);

CREATE TABLE ab_pm_char_ids (
    `char_id` INT UNSIGNED NOT NULL,
    `ab_char_id` INT UNSIGNED NOT NULL,
    FOREIGN KEY (char_id) REFERENCES ab_common_config(char_id)
);

ALTER TABLE `ab_pm_char_ids` ADD UNIQUE KEY `char_ab_unique` (`char_id`, `ab_char_id`);

ALTER TABLE `ab_skills`
DROP INDEX `char_id`,
ADD UNIQUE KEY `char_id` (`char_id`, `type`, `skill_id`, `skill_lv`);

commit;