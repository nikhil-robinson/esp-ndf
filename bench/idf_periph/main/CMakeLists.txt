idf_component_register(SRCS "main.c" "t_gpio.c" "t_gptimer.c" "t_ledc.c" "t_mcpwm.c" "t_pcnt.c" INCLUDE_DIRS "." PRIV_REQUIRES driver esp_timer)
