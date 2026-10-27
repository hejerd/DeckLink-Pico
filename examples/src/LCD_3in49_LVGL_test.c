/*****************************************************************************
* | File        :   LCD_3IN49_LVGL_test.c
* | Author      :   Waveshare team
* | Function    :   test Demo
* | Info        :
*----------------
* |	This version:   V1.0
* | Date        :   2025-03-13
* | Info        :   
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documnetation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to  whom the Software is
# furished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS OR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
#
******************************************************************************/
#include "LCD_test.h"
#include "LCD_3in49.h"
#include "qspi_pio.h"
#include "QMI8658.h"
#include "Touch.h"
#include "PCF85063A.h"
#include "pico/multicore.h"  

void core1_entry();

int LCD_3IN49_LVGL_Test(void)
{
    if (DEV_Module_Init() != 0)
    {
      return -1;
    } 
    
    /*PWR*/
    multicore_launch_core1(core1_entry);

    printf("LCD_3IN49_LCGL_test Demo\r\n");
    /*QSPI PIO Init*/
    QSPI_GPIO_Init(qspi);
    QSPI_PIO_Init(qspi);
    QSPI_4Wrie_Mode(&qspi);
    /*Init LCD*/
    LCD_3IN49_Init();
    DEV_SET_PWM(60);
    LCD_3IN49_Clear(WHITE);
    /*Init RTC*/
    PCF85063A_Init();
    /*Init IMU*/
    QMI8658_init();
    /*Init LVGL*/
    LVGL_Init();
    Widgets_Init();

    while(1)
    {
      lv_task_handler();
      DEV_Delay_ms(5); 
    }

    DEV_Module_Exit();
    return 0;
}

void core1_entry() {
    static int press_time = 0;
    while(1)
    {
        DEV_Delay_ms(5); 
        if(DEV_Digital_Read(SYS_OUT) == 0)
        {
            press_time++;
            if(press_time > 300)//shutdown  
            {
                press_time = 0;
                DEV_Digital_Write(SYS_EN, 0);
            }
        }
        else
        {
            press_time = 0;
        }
    }
}
