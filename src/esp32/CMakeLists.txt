# ESP-IDF Component CMakeLists.txt
idf_component_register(SRCS "main.c"
                            "../ikcp.c"
                            "../encrypt.c"
                            "../socks5.c"
                            "../tunnel.c"
                       INCLUDE_DIRS "." "../"
                       REQUIRES lwip mbedtls)
