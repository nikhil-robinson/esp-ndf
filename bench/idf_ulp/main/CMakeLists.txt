idf_component_register(SRCS "main.c" INCLUDE_DIRS "." "../../ulp" REQUIRES ulp esp_hw_support)
ulp_embed_binary(ulp_main "ulp/main.c" "main.c")
