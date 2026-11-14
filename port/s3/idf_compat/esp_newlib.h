/* esp_newlib.h —— 最小壳。
 * IDF 自己的版本在 components/esp_libc/platform_include/ 里，带有整套 newlib
 * 覆盖头（stdio.h/time.h…），会和我们工具链自带 newlib 冲突。
 * 我们需要的只有 FreeRTOS tasks.c 用到的 esp_reent_init()，而且我们不编译
 * tasks.c，所以这里给个空壳即可。 */
#ifndef C6OS_ESP_NEWLIB_H
#define C6OS_ESP_NEWLIB_H

void esp_reent_init(void *reent_ptr);

#endif
