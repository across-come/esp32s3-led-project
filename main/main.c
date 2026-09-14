#include "user_ext.h"

void app_main(void)
{
    user_init();                /* 只注册消息/驱动, 不自动连 STA/TCP、不挂 SD */

    while(1)
    {
        task_proc();            /* 不延时: 看门狗IDLE0会叫, 本次按要求不管 */
    }
}