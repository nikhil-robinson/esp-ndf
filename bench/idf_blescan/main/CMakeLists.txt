idf_component_register(SRCS "main.c" PRIV_REQUIRES bt nvs_flash)
if(DEFINED ENV{ADV})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ADV=1)
endif()
