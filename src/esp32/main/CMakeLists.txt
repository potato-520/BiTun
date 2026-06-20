# Component CMakeLists.txt for ESP-IDF
# We register the parent main.c, the dummy entrypoint, and the shared source files from grandfather src/
idf_component_register(SRCS "../main.c"
                            "app_main_dummy.c"
                            "../../ikcp.c"
                            "../../encrypt.c"
                            "../../socks5.c"
                            "../../tunnel.c"
                            "../../fec.c"
                       INCLUDE_DIRS "../" "../../"
                       REQUIRES lwip mbedtls)

# Suppress format warning errors and incompatible pointer types warnings
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-error=format" "-Wno-error=incompatible-pointer-types")
