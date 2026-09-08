// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

/**
 * Custom battle config initializations
 * Place the definition of custom battle configuration initializations in this file.
 *
 * Do not forget to add a comma at the end of the line, if you want to add another config
 *
 * Format:
 *	 { "name", &battle_config.<variable name>, <default value>, <minimum value>, <maximum value> },
 **/




{ "population_engine_max_count",&battle_config.population_engine_max_count,1000,1,30000,},
{ "population_engine_equipment_strict_load",&battle_config.population_engine_equipment_strict_load,0,0,1,},
{ "population_engine_chat_enable",&battle_config.population_engine_chat_enable,0,0,1,},
{ "population_engine_chat_tick_ms",&battle_config.population_engine_chat_tick_ms,1500,500,600000,},
{ "population_engine_chat_cooldown_ms",&battle_config.population_engine_chat_cooldown_ms,15000,1000,3600000,},
{ "population_engine_chat_cooldown_jitter_ms",&battle_config.population_engine_chat_cooldown_jitter_ms,5000,0,600000,},
{ "population_engine_chat_max_per_tick",&battle_config.population_engine_chat_max_per_tick,10,0,500,},
{ "population_engine_chat_reply_enable",&battle_config.population_engine_chat_reply_enable,0,0,1,},
{ "population_engine_chat_speak_chance",&battle_config.population_engine_chat_speak_chance,8,0,100,},
{ "population_engine_allmap_field_pct",&battle_config.population_engine_allmap_field_pct,40,0,100,},
{ "population_engine_allmap_dungeon_pct",&battle_config.population_engine_allmap_dungeon_pct,35,0,100,},
{ "population_engine_wander_enable",&battle_config.population_engine_wander_enable,1,0,1,},
{ "population_engine_wander_tick_ms",&battle_config.population_engine_wander_tick_ms,500,200,600000,},
{ "population_engine_wander_radius",&battle_config.population_engine_wander_radius,12,1,50,},
{ "population_engine_wander_cooldown_ms",&battle_config.population_engine_wander_cooldown_ms,2000,0,600000,},
{ "population_engine_wander_cooldown_jitter_ms",&battle_config.population_engine_wander_cooldown_jitter_ms,1500,0,600000,},
{ "population_engine_wander_max_per_tick",&battle_config.population_engine_wander_max_per_tick,50,1,500,},
{ "population_engine_hat_effect",&battle_config.population_engine_hat_effect,0,0,INT_MAX,},
{ "population_engine_shell_move_min",&battle_config.population_engine_shell_move_min,7,0,500,},
{ "population_engine_shell_move_max",&battle_config.population_engine_shell_move_max,20,0,500,},
{ "population_engine_shell_timer_ms",&battle_config.population_engine_shell_timer_ms,100,10,60000,},
{ "population_engine_combat_budget_per_tick",&battle_config.population_engine_combat_budget_per_tick,60,1,5000,},
{ "population_engine_shell_mobtracker_interval_ms",&battle_config.population_engine_shell_mobtracker_interval_ms,200,100,600000,},
{ "population_engine_shell_mdetection_cells",&battle_config.population_engine_shell_mdetection_cells,30,1,100,},
{ "population_engine_shell_detection_cache_ms",&battle_config.population_engine_shell_detection_cache_ms,300,0,600000,},
{ "population_engine_shell_movement_owner_sm",&battle_config.population_engine_shell_movement_owner_sm,1,0,1,},
{ "population_engine_shell_attack_skill_delay_ms",&battle_config.population_engine_shell_attack_skill_delay_ms,100,0,60000,},
{ "population_engine_shell_attackskill",&battle_config.population_engine_shell_attackskill,1,0,1,},
{ "population_engine_shell_basic_attack_chance",&battle_config.population_engine_shell_basic_attack_chance,30,0,100,},
{ "population_engine_shell_skill_min_sp_pct",&battle_config.population_engine_shell_skill_min_sp_pct,15,0,99,},
{ "population_engine_shell_sticky_target_ms",&battle_config.population_engine_shell_sticky_target_ms,2000,0,60000,},
{ "population_engine_shell_skill_los_check",&battle_config.population_engine_shell_skill_los_check,1,0,1,},
{ "population_engine_shell_skill_strict_gate",&battle_config.population_engine_shell_skill_strict_gate,1,0,1,},
{ "population_engine_name_bot_fallback",&battle_config.population_engine_name_bot_fallback,0,0,1,},
{ "population_engine_combat_max_budget",&battle_config.population_engine_combat_max_budget,200,1,5000,},
{ "population_engine_shell_skill_interval_ms",&battle_config.population_engine_shell_skill_interval_ms,200,0,60000,},
{ "population_engine_autosummon_batch_size",&battle_config.population_engine_autosummon_batch_size,50,0,5000,},
{ "population_engine_autosummon_enable",&battle_config.population_engine_autosummon_enable,0,0,1,},
{ "population_engine_ai",&battle_config.population_engine_ai,0x1FF,0x000,0x3FF,},
{ "population_engine_daily_cycle",&battle_config.population_engine_daily_cycle,0,0,1,},
{ "population_engine_vending_enable",&battle_config.population_engine_vending_enable,0,0,1,},
{ "population_engine_arena_custom_pool",&battle_config.population_engine_arena_custom_pool,0,0,1,},
{ "population_engine_path_attempts",&battle_config.population_engine_path_attempts,16,1,256,},

/**
 * Extended Vending system [Lilith / Easycore]
 **/
{ "extended_vending",     &battle_config.extended_vending,     1, 0, 1,          },
{ "show_broadcast_info",  &battle_config.show_broadcast_info,  1, 0, 1,          },
{ "show_item_vending",    &battle_config.show_item_vending,    1, 0, 1,          },
{ "ex_vending_info",      &battle_config.ex_vending_info,      1, 0, 1,          },
{ "ex_vending_report",    &battle_config.ex_vending_report,    1, 0, 1,          },
{ "ex_buying_bound",      &battle_config.ex_buying_bound,      0, 0, 1,          },
{ "item_zeny",            &battle_config.item_zeny,            0, 0, INT32_MAX,  },
{ "item_cash",            &battle_config.item_cash,            0, 0, INT32_MAX,  },
