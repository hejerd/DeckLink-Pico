#include "LCD_test.h"  //example

int main(void)
{
    stdio_init_all();
    sleep_ms(3000);          // 터미널 연결 대기
    printf("boot\n");
    LCD_3IN49_LVGL_Test();
    return 0;
}