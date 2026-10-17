idf_component_register(SRCS "main.c" "t_gpio.c" "t_gptimer.c" "t_ledc.c" "t_mcpwm.c" "t_pcnt.c" "t_rmt.c" "t_sdm.c" "t_uart.c" "t_i2c.c" INCLUDE_DIRS "." PRIV_REQUIRES driver esp_timer)
