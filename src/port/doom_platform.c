/*
 * Doom platform glue for the EK-RA8P1:
 *   - doomgeneric DG_* hooks (timing, sleeping)
 *   - SDRAM memory arena (zone heap, IWAD copy, RAM file system)
 *   - memory mapped WAD file class (lumps are used in place, no copies)
 *   - quit / fatal error handling
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hal_data.h"
#include "FreeRTOS.h"
#include "task.h"

#include "ra8_port.h"

#include "doomgeneric.h"
#include "doomtype.h"
#include "w_file.h"
#include "z_zone.h"

/* ------------------------------------------------------------------------- */
/* SDRAM arena                                                                */
/* ------------------------------------------------------------------------- */

#define SDRAM_ARENA_BYTES  (RA8_ZONE_BYTES + RA8_WAD_MAX_BYTES + RA8_RAMFS_POOL_BYTES + (64U * 1024U))

/* Cached, not zero initialised (the zone allocator initialises itself). */
static uint8_t g_sdram_arena[SDRAM_ARENA_BYTES] BSP_PLACE_IN_SECTION(".sdram_noinit") BSP_ALIGN_VARIABLE(64);
static size_t  g_sdram_used = 0;

void * sdram_alloc (size_t size)
{
    size = (size + 63U) & ~(size_t) 63U;

    if ((g_sdram_used + size) > sizeof(g_sdram_arena))
    {
        return NULL;
    }

    void * p = &g_sdram_arena[g_sdram_used];
    g_sdram_used += size;

    return p;
}

/* Called from I_ZoneBase() (patched i_system.c). */
byte * RA8_ZoneBase (int * size)
{
    static byte * p_zone = NULL;

    if (NULL == p_zone)
    {
        p_zone = sdram_alloc(RA8_ZONE_BYTES);
    }

    *size = (int) RA8_ZONE_BYTES;

    return p_zone;
}

/* ------------------------------------------------------------------------- */
/* doomgeneric hooks                                                          */
/* ------------------------------------------------------------------------- */

void DG_Init (void)
{
}

void DG_DrawFrame (void)
{
}

void DG_SleepMs (uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);

    vTaskDelay((ticks > 0U) ? ticks : 1U);
}

uint32_t DG_GetTicksMs (void)
{
    return (uint32_t) (xTaskGetTickCount() * portTICK_PERIOD_MS);
}

int DG_GetKey (int * pressed, unsigned char * key)
{
    (void) pressed;
    (void) key;

    return 0;                          /* Input is delivered by i_input_ra8.c */
}

void DG_SetWindowTitle (const char * title)
{
    printf("DOOM: %s\n", title);
}

/* ------------------------------------------------------------------------- */
/* Memory mapped WAD file class (replaces w_file_stdc.c)                      */
/* ------------------------------------------------------------------------- */

extern wad_file_class_t stdc_wad_file;

static wad_file_t * W_Mem_OpenFile (char * path)
{
    size_t          size;
    const uint8_t * p_data = ramfs_get_file_data(path, &size);

    if (NULL == p_data)
    {
        return NULL;
    }

    wad_file_t * p_wad = Z_Malloc(sizeof(wad_file_t), PU_STATIC, 0);

    p_wad->file_class = &stdc_wad_file;
    p_wad->mapped     = (byte *) (uintptr_t) p_data;
    p_wad->length     = (unsigned int) size;

    return p_wad;
}

static void W_Mem_CloseFile (wad_file_t * wad)
{
    Z_Free(wad);
}

static size_t W_Mem_Read (wad_file_t * wad, unsigned int offset, void * buffer, size_t buffer_len)
{
    if (offset >= wad->length)
    {
        return 0;
    }

    if (buffer_len > (wad->length - offset))
    {
        buffer_len = wad->length - offset;
    }

    memcpy(buffer, wad->mapped + offset, buffer_len);

    return buffer_len;
}

wad_file_class_t stdc_wad_file =
{
    W_Mem_OpenFile,
    W_Mem_CloseFile,
    W_Mem_Read,
};

/* ------------------------------------------------------------------------- */
/* Quit and fatal errors                                                      */
/* ------------------------------------------------------------------------- */

static void restart_board (uint32_t delay_ms)
{
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    NVIC_SystemReset();
}

/* Called from I_Quit() (patched i_system.c) after Doom saved its config. */
void RA8_Quit (void)
{
    printf("DOOM: quit requested, restarting\n");
    ra8_video_boot_message("THANKS FOR PLAYING", "RESTARTING...");
    restart_board(1500);
}

/* No command interpreter (the engine only uses this for a desktop error
 * dialog, which -nogui disables). Defining it here keeps picolibc's version,
 * which needs fork/exec, out of the link. */
int system (const char * command)
{
    (void) command;

    return -1;
}

/* exit() / I_Error() end up here. */
void _exit (int status)
{
    printf("DOOM: exit(%d)\n", status);

    if (0 != status)
    {
        ra8_video_boot_message("DOOM HAS STOPPED", rtt_log_last_line());

        /* Stop here if a debugger is attached so the state can be inspected. */
        if (CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk)
        {
            __BKPT(0);
        }

        restart_board(10000);
    }

    restart_board(500);

    for (;;)
    {
        /* NVIC_SystemReset() does not return. */
    }
}
