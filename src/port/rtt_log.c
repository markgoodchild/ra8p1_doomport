/*
 * Minimal SEGGER RTT (Real Time Transfer) implementation, used as the
 * stdout/stderr console of the Doom port.
 *
 * View the output with "JLinkRTTViewer" or "JLinkRTTClient" while the
 * debugger (or JLink.exe) is connected. The control block lives in
 * non-cacheable RAM so the debug probe always sees coherent data while the
 * Cortex-M85 data cache is enabled.
 */
#include <stdio.h>
#include <string.h>

#include "bsp_api.h"
#include "ra8_port.h"

#define RTT_UP_BUFFER_SIZE    (16U * 1024U)
#define RTT_DOWN_BUFFER_SIZE  (16U)

/* RTT ring buffer descriptor (layout defined by SEGGER). */
typedef struct
{
    const char        * p_name;
    char              * p_buffer;
    uint32_t            size;
    volatile uint32_t   wr_off;
    volatile uint32_t   rd_off;
    uint32_t            flags;
} rtt_buffer_t;

/* RTT control block (layout defined by SEGGER). */
typedef struct
{
    char         id[16];
    int32_t      max_up;
    int32_t      max_down;
    rtt_buffer_t up[1];
    rtt_buffer_t down[1];
} rtt_cb_t;

/* RTT flags: 0 = skip data that does not fit (never block the game). */
#define RTT_MODE_NO_BLOCK_SKIP  (0U)

static rtt_cb_t g_rtt_cb BSP_PLACE_IN_SECTION(".ram_nocache") BSP_ALIGN_VARIABLE(32);
static char     g_rtt_up_buf[RTT_UP_BUFFER_SIZE] BSP_PLACE_IN_SECTION(".ram_nocache");
static char     g_rtt_down_buf[RTT_DOWN_BUFFER_SIZE] BSP_PLACE_IN_SECTION(".ram_nocache");

static bool g_rtt_ready = false;

void rtt_log_init (void)
{
    if (g_rtt_ready)
    {
        return;
    }

    memset(&g_rtt_cb, 0, sizeof(g_rtt_cb));
    g_rtt_cb.max_up   = 1;
    g_rtt_cb.max_down = 1;

    g_rtt_cb.up[0].p_name   = "Terminal";
    g_rtt_cb.up[0].p_buffer = g_rtt_up_buf;
    g_rtt_cb.up[0].size     = RTT_UP_BUFFER_SIZE;
    g_rtt_cb.up[0].flags    = RTT_MODE_NO_BLOCK_SKIP;

    g_rtt_cb.down[0].p_name   = "Terminal";
    g_rtt_cb.down[0].p_buffer = g_rtt_down_buf;
    g_rtt_cb.down[0].size     = RTT_DOWN_BUFFER_SIZE;
    g_rtt_cb.down[0].flags    = RTT_MODE_NO_BLOCK_SKIP;

    /* Write the ID last, in two steps, so that the debugger never finds a
     * partially initialised control block (and never matches a copy of the
     * string stored in flash). */
    static const char id_hi[] = "RTT";
    static const char id_lo[] = "SEGGER ";
    memcpy(&g_rtt_cb.id[7], id_hi, sizeof(id_hi));
    __DMB();
    memcpy(&g_rtt_cb.id[0], id_lo, sizeof(id_lo) - 1U);
    __DMB();

    g_rtt_ready = true;
}

void rtt_log_write (const char * p_data, size_t len)
{
    if (!g_rtt_ready)
    {
        rtt_log_init();
    }

    rtt_buffer_t * p_up = &g_rtt_cb.up[0];

    /* The console may be written from more than one task; keep it simple
     * and make the ring buffer update atomic. */
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    uint32_t wr = p_up->wr_off;
    uint32_t rd = p_up->rd_off;

    for (size_t i = 0; i < len; i++)
    {
        uint32_t next = wr + 1U;
        if (next >= p_up->size)
        {
            next = 0U;
        }

        if (next == rd)
        {
            break;                     /* Buffer full: drop the remainder. */
        }

        p_up->p_buffer[wr] = p_data[i];
        wr                 = next;
    }

    __DMB();
    p_up->wr_off = wr;

    __set_PRIMASK(primask);
}

/* ------------------------------------------------------------------------- */
/* picolibc console streams                                                   */
/* ------------------------------------------------------------------------- */

/* Copy of the most recent non-empty console line, shown on screen if the
 * game stops with an error. */
static char   g_cur_line[96];
static size_t g_cur_len;
static char   g_last_line[96];

const char * rtt_log_last_line (void)
{
    return g_last_line;
}

static void track_line (char c)
{
    if ('\n' == c)
    {
        if (g_cur_len > 0U)
        {
            memcpy(g_last_line, g_cur_line, g_cur_len);
            g_last_line[g_cur_len] = '\0';
        }

        g_cur_len = 0;
    }
    else if ((c >= ' ') && (g_cur_len < (sizeof(g_cur_line) - 1U)))
    {
        g_cur_line[g_cur_len++] = c;
    }
    else
    {
        /* Ignore control characters and overlong lines. */
    }
}

static int rtt_putc (char c, FILE * p_file)
{
    (void) p_file;

    track_line(c);

    if ('\n' == c)
    {
        rtt_log_write("\r\n", 2);
    }
    else
    {
        rtt_log_write(&c, 1);
    }

    return (unsigned char) c;
}

static int rtt_getc (FILE * p_file)
{
    (void) p_file;

    return _FDEV_EOF;
}

static FILE g_rtt_stdio = FDEV_SETUP_STREAM(rtt_putc, rtt_getc, NULL, _FDEV_SETUP_RW);

FILE * const stdin  = &g_rtt_stdio;
FILE * const stdout = &g_rtt_stdio;
FILE * const stderr = &g_rtt_stdio;
