// app_main_dummy.c
#include <stdio.h>

extern void bitun_esp32_init(void);

void app_main(void) {
    printf("[Dummy] Starting ESP-IDF app_main...\n");
    bitun_esp32_init();
}
