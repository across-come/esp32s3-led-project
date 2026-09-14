#include "user_ext.h"

/* ============ UART 异步发送队列 ============ */
void uart_write(unsigned char *buf, unsigned int len)
{
    struct st_kv *p_kv;
    struct st_uart_tx_arg *p_arg;

    if((buf == NULL) || (len == 0))
        return;

    if((p_kv = kv_add(sizeof(struct st_uart_tx_arg) + len)) == NULL)
        return;

    p_arg = (struct st_uart_tx_arg*)p_kv->dat;
    p_arg->port = USART_UX;
    p_arg->buf = (unsigned char*)(p_arg + 1);
    p_arg->idx = 0;
    p_arg->len = len;
    p_arg->tmr = tick_get();
    memcpy(p_arg->buf, buf, len);
}

void dbgtx(char *fmt, ...)
{
    char            buf[256];
    unsigned int    len;
    va_list         ap;

    va_start(ap, fmt);
    len = sprintf(buf, "%d ", tick_get());
    len += vsnprintf(buf + len, sizeof(buf) - len, fmt, ap);
    va_end(ap);

    if(len >= sizeof(buf))
        len = sizeof(buf) - 1;

    uart_write((unsigned char*)buf, len);
}

void print_version(void)
{
    dbgtx("\r\n\r\n****************************\r\n");
    dbgtx("    %s %s\r\n", __DATE__, __TIME__);
    dbgtx("****************************\r\n");
}

/* ============ MSG ============ */
unsigned int msg_send(char *name, char *dat)
{
    unsigned int idx;

    for(idx = 0; idx < sys.msg_cnt; idx++)
    {
        if((sys.msg[idx].name != NULL) && (strcmp(sys.msg[idx].name, name) == 0))
        {
            if(sys.msg[idx].func != NULL)
                return sys.msg[idx].func(dat);
            return c_ret_nk;
        }
    }
    return c_ret_nk;
}

unsigned int msg_add(char *name, msg_func func)
{
    unsigned int idx;

    while(1)
    {
        for(idx = 0; idx < sys.msg_cnt; idx++)
        {
            if(sys.msg[idx].name == NULL)
            {
                sys.msg[idx].name = name;
                sys.msg[idx].func = func;
                return c_ret_ok;
            }
        }
        if((sys.msg = (struct st_msg*)bsp_realloc(sys.msg, sizeof(struct st_msg) * (sys.msg_cnt + 1))) == NULL)
            return c_ret_nk;
        sys.msg_cnt++;
    }
}

/* ============ VAR ============ */
unsigned int var_add(char *name, unsigned int addr, unsigned int type)
{
    while(1)
    {
        for(int idx = 0; idx < sys.var_cnt; idx++)
        {
            if(sys.var[idx].name == NULL)
            {
                sys.var[idx].name = name;
                sys.var[idx].addr = addr;
                sys.var[idx].type = type;
                return c_ret_ok;
            }
        }
        if((sys.var = (struct st_var*)bsp_realloc(sys.var, sizeof(struct st_var) * (sys.var_cnt + 1))) == NULL)
            return c_ret_nk;
        sys.var_cnt++;
    }
}

/* ============ EXE ============ */
unsigned int exe_add(char *name, char *desc, exe_func func)
{
    while(1)
    {
        for(int idx = 0; idx < sys.exe_cnt; idx++)
        {
            if(sys.exe[idx].name == NULL)
            {
                sys.exe[idx].name = name;
                sys.exe[idx].desc = desc;
                sys.exe[idx].func = func;
                return c_ret_ok;
            }
        }
        if((sys.exe = (struct st_exe*)bsp_realloc(sys.exe, sizeof(struct st_exe) * (sys.exe_cnt + 1))) == NULL)
            return c_ret_nk;
        sys.exe_cnt++;
    }
}

/* ============ DBG 命令 ============ */
static unsigned int dbg_parse_var(char *buf)
{
	char 				name_str[32], val_str[16];
	unsigned int 		idx, val, typ, addr, wx;
	double				dx;

    if(sscanf(buf, "%[^,],%s", name_str, val_str) != 2)
	{
        return c_ret_nk;
	}

	for(idx = 0; idx < sys.var_cnt; idx++)
	{
		if((sys.var[idx].name != NULL) && (strcmp(sys.var[idx].name, name_str) != 0))
			continue;

		addr = sys.var[idx].addr;
		typ = sys.var[idx].type;
		if(val_str[0] == '?')
		{
			switch(typ)
			{
				case 1:		dbgtx("var %s=%f OK\r\n",       sys.var[idx].name, *(float*)addr); break;
				case 2:		dbgtx("var %s=%lf OK\r\n",      sys.var[idx].name, *(double*)addr); break;
				case 7:		dbgtx("var %s=0x%x(%d) OK\r\n", sys.var[idx].name, *(char*)addr, 			*(char*)addr); break;
				case 8:		dbgtx("var %s=0x%x(%d) OK\r\n", sys.var[idx].name, *(unsigned char*)addr,	*(unsigned char*)addr); break;
				case 15:	dbgtx("var %s=0x%x(%d) OK\r\n", sys.var[idx].name, *(short*)addr, 			*(short*)addr); break;
				case 16:	dbgtx("var %s=0x%x(%d) OK\r\n", sys.var[idx].name, *(unsigned short*)addr,	*(unsigned short*)addr); break;
				case 31:	dbgtx("var %s=0x%x(%d) OK\r\n", sys.var[idx].name, *(int*)addr, 			*(int*)addr); break;
				case 32:	dbgtx("var %s=0x%x(%d) OK\r\n", sys.var[idx].name, *(unsigned int*)addr, 	*(unsigned int*)addr); break;
				default:	return c_ret_nk;
			}
			return c_ret_ok;
		}
		else
		{
			if(str2num(val_str, &wx, &dx) != c_ret_ok)
				return c_ret_nk;

			switch(typ)
			{
				case 1:		*(float*)addr = (float)dx; break;
				case 2:		*(double*)addr = dx; break;
				case 7:		*(char*)addr = (char)wx; break;
				case 8:		*(unsigned char*)addr = (unsigned char)wx; break;
				case 15:	*(short*)addr = (short)wx; break;
				case 16:	*(unsigned short*)addr = (unsigned short)wx; break;
				case 31:	*(int*)addr = (int)wx; break;
				case 32:	*(unsigned int*)addr = wx; break;
				default: 	return c_ret_nk;
			}
			dbgtx("var %s,%s OK\r\n", name_str, val_str);
			return c_ret_ok;
		}
	}

	return c_ret_nk;
}

static unsigned int dbg_parse_exe(char *buf)
{
	unsigned int    arg_begin;
	char		    name_str[32];

	arg_begin = 0;
	if((sscanf(buf, "%31[^(]%n", name_str, &arg_begin) != 1)&&(arg_begin == 0))
		return c_ret_nk;

	for(int idx = 0; idx < sys.exe_cnt; idx++)
	{
		if((sys.exe[idx].name != NULL)&&(strcmp(sys.exe[idx].name, name_str) == 0))
		{
            dbgtx("exe=%s ok\r\n",buf);
			return sys.exe[idx].func(buf + arg_begin);
		}
	}
	return c_ret_nk;
}

static unsigned int dbg_msg_send(char *buf)
{
    char    msg_name[32], dat[96];
    char    *p_msg, *p_dat, *p_end;
    unsigned int len;

    if(buf == NULL) return c_ret_nk;

    p_msg = strstr(buf, "(msg:");
    p_dat = strstr(buf, ",dat:");
    p_end = strrchr(buf, ')');
    if((p_msg != buf) || (p_dat == NULL) || (p_end == NULL) || (p_dat <= p_msg) || (p_end <= p_dat))
    {
        dbgtx("%s parse nk\r\n", buf);
        return c_ret_nk;
    }

    p_msg += 5;
    len = (unsigned int)(p_dat - p_msg);
    if(len >= sizeof(msg_name)) len = sizeof(msg_name) - 1;
    memcpy(msg_name, p_msg, len);
    msg_name[len] = 0;

    p_dat += 5;
    len = (unsigned int)(p_end - p_dat);
    if(len >= sizeof(dat)) len = sizeof(dat) - 1;
    memcpy(dat, p_dat, len);
    dat[len] = 0;

    return msg_send(msg_name, dat);
}

static unsigned int dbg_port_get(char *buf)
{
    int gpio, pin, val;

    if(sscanf(buf, "(gpio:%d,pin:%d)", &gpio, &pin) != 2)
    {
        dbgtx_fl("sscanf error\r\n");
        return c_ret_nk;
    }
    if((gpio < 0) || (gpio > 48))
    {
        dbgtx_fl("parameter error\r\n");
        return c_ret_nk;
    }
    val = gpio_get_level((gpio_num_t)gpio);
    dbgtx_fl("dbg_port_get%s=%d\r\n", buf, val);
    return c_ret_ok;
}

static unsigned int dbg_print_task_name(char *buf)
{
	dbgtx("******** task_name ********\r\n");
	for(int idx = 0; idx < sys.task_cnt; idx++)
	{
        if(sys.task[idx].name != NULL)
            dbgtx("    %s step:%d done:%d wait:%d tmr:%u\r\n", sys.task[idx].name, sys.task[idx].step, sys.task[idx].done, sys.task[idx].wait, sys.task[idx].tmr);
	}
    return c_ret_ok;
}

static unsigned int dbg_print_msg_name(char *buf)
{
	dbgtx("******** msg_name ********\r\n");
	for(int idx = 0; idx < sys.msg_cnt; idx++)
	{
        if(sys.msg[idx].name != NULL)
		    dbgtx("    %s\r\n", sys.msg[idx].name);
	}
    return c_ret_ok;
}

static void print_var_name_addr(void)
{
	dbgtx("******** var_name_addr ********\r\n");
	for(int idx = 0; idx < sys.var_cnt; idx++)
	{
        if(sys.var[idx].name != NULL)
		    dbgtx("    %s,0x%x\r\n", sys.var[idx].name, sys.var[idx].addr);
	}
}

static void print_exe_name_desc(void)
{
	dbgtx("******** exe_name_desc ********\r\n");
	for(int idx = 0; idx < sys.exe_cnt; idx++)
	{
        if(sys.exe[idx].name != NULL)
		    dbgtx("    %s%s\r\n", sys.exe[idx].name, sys.exe[idx].desc);
	}
}

unsigned int dbg_parse_trace(char *buf)
{
#if c_trace_en == 1
    if(strcmp(buf, "null") == 0)
    {
        user_trace.mode = c_trace_mode_idle;
    }
    else if(strcmp(buf, "call") == 0)
    {
        struct st_trace_call *p_call = &user_trace.ctl.call;

        p_call->head = 0;
        p_call->print_req = 0;
        user_trace.mode = c_trace_mode_call;
    }
    else if(strstr(buf, "tmo,") != NULL)
    {
        struct st_trace_tmo *p_tmo = &user_trace.ctl.tmo;
        unsigned int tmo;

        if(sscanf(buf,"tmo,%d", &tmo) != 1)
            return c_ret_nk;

        p_tmo->tmo = tmo;
        p_tmo->tmr = dwt_get_us();
        p_tmo->prev_func = p_tmo->last_func = (char*)__func__;
        user_trace.mode = c_trace_mode_tmo;
    }
    else if(strstr(buf,"var,") != NULL)
    {
        struct st_trace_var *p_var = &user_trace.ctl.var;
        unsigned int addr, typ;

        if(sscanf(buf,"var,%x,%d", &addr, &typ) != 2)
            return c_ret_nk;

        switch(typ)
        {
            case 1:     p_var->raw_val.f   = *(float*)addr; break;
            case 2:     p_var->raw_val.d   = *(double*)addr; break;
            case 8:     p_var->raw_val.b8  = *(unsigned char*)addr; break;
            case 16:    p_var->raw_val.b16 = *(unsigned short*)addr; break;
            case 32:    p_var->raw_val.b32 = *(unsigned int*)addr; break;
            default: return c_ret_nk;
        }
		p_var->new_val.d = p_var->raw_val.d;
        p_var->addr = addr;
        p_var->typ = typ;
		p_var->prev_func = p_var->last_func = (char*)__func__;
        user_trace.mode = c_trace_mode_var;
    }
    else return c_ret_nk;

    dbgtx("trace=%s OK\r\n", buf);
    return c_ret_ok;
#else
    dbgtx("trace=%s c_trace_en=0\r\n", buf);
    return c_ret_ok;
#endif
}

static void dbg_parse(char *buf)
{
    char *px;

    if(strcmp(buf, "help") == 0)
    {
        dbgtx("********  cmd sets  ********\r\n");
        dbgtx("1. help\r\n");
        dbgtx("2. version\r\n");
        dbgtx("3. exe?\r\n");
        dbgtx("4. var=name,?/val\r\n");
        dbgtx("5. exe=func(arg_json)\r\n");
        dbgtx("6. trace=call trace=tmo,dd trace=var,addr,typ\r\n");

        return;
    }
    else if(strcmp(buf, "version") == 0)
    {
        print_version();
        return;
    }
    else if(strcmp(buf, "var?") == 0)
    {
        print_var_name_addr();
        return;
    }
    else if(strcmp(buf, "exe?") == 0)
    {
        print_exe_name_desc();
        return;
    }
     else if((px = strstr(buf, "var=")) != NULL)
    {
        if((px == buf)&&(dbg_parse_var(px + 4) == c_ret_ok))
            return;
    }
    else if((px = strstr(buf, "exe=")) != NULL)
    {
        if((px == buf)&&(dbg_parse_exe(px + 4) == c_ret_ok))
            return;
    }
    else if((px = strstr(buf, "trace=")) != NULL)
    {
        if((px == buf)&&(dbg_parse_trace(px + 6) == c_ret_ok))
            return;
    }
    dbgtx("%s nk\r\n", buf);
}

/* UART接收任务: 由task_proc周期调用, 轮询读串口按行解析, 无需环形缓冲 */
static void task_rx(void)
{
    static unsigned char data, buf[128];
    static unsigned int idx = 0;

    while(uart_read_bytes(USART_UX, &data, 1, 0) > 0)
    {
        if((data == '\r') || (data == '\n'))
        {
            if(idx != 0)
            {
                buf[idx] = 0;
                dbg_parse((char*)buf);
            }
            idx = 0;
        }
        else if(idx < sizeof(buf) - 1)
            buf[idx++] = data;
    }
}

static void task_tx(void)
{
    static unsigned int step = 0;
    static unsigned int idx = 0;
    struct st_kv *p_kv = NULL;
    struct st_uart_tx_arg *p_arg;

    switch(step)
    {
        case 0:
            for(idx = 0; idx < sys.kv_cnt; idx++)
            {
                p_kv = &sys.kv[idx];
                if(p_kv->dat == NULL)
                    continue;

                p_arg = (struct st_uart_tx_arg*)p_kv->dat;
                uart_write_bytes(p_arg->port, (const char*)p_arg->buf, p_arg->len);
                kv_del(p_kv);
                step = 0;
                return;
            }
            step = 0;
        break;

        default:
            step = 0;
        break;
    }
}

void bsp_init(void)
{
    exe_add("dbg_msg_send","(msg:xxx_msg,dat:yyy_dat)", dbg_msg_send);
    exe_add("dbg_port_get", "(gpio:x,pin:y)", dbg_port_get);
    exe_add("dbg_print_msg", "()", dbg_print_msg_name);
    exe_add("dbg_print_task", "()", dbg_print_task_name);
	#if c_trace_en == 1
	var_add("trace_print_req", (unsigned int)&user_trace.ctl.call.print_req, 32);
	#endif
}
INIT_REG(bsp_init, 1);

void uart_init(void)
{
    uart_config_t uart0_config = {0};

    uart0_config.baud_rate = 115200;
    uart0_config.data_bits = UART_DATA_8_BITS;
    uart0_config.parity = UART_PARITY_DISABLE;
    uart0_config.stop_bits = UART_STOP_BITS_1;
    uart0_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart0_config.source_clk = UART_SCLK_APB;
    uart0_config.rx_flow_ctrl_thresh = 122;

    ESP_ERROR_CHECK(uart_param_config(USART_UX, &uart0_config));
    ESP_ERROR_CHECK(uart_set_pin(USART_UX, USART_TX_GPIO_PIN, USART_RX_GPIO_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(USART_UX, 256, 256, 0, NULL, 0));

    task_add("task_rx", task_rx, c_auto_quit, 0);

    task_add("task_tx", task_tx, c_auto_wait, 0);
}
INIT_REG(uart_init, 1);