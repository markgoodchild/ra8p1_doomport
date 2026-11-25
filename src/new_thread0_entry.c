/*
 * Doom for the Renesas EK-RA8P1 - main game thread.
 *
 * Boot sequence:
 *   1. Start the RTT console and the display (boot screen).
 *   2. Start the USB host tasks: PS5 / PS4 controller (USBHS) and USB
 *      headphones (USBFS).
 *   3. Copy the IWAD from OSPI flash into SDRAM.
 *   4. Run the Doom engine (doomgeneric) forever.
 */
#include <stdio.h>
#include <string.h>

#include "new_thread0.h"

#include "ra8_port.h"
#include "doomgeneric.h"

/* Pick the canonical IWAD file name from the lumps it contains, so the engine
 * identifies the game correctly (shareware, registered/Ultimate or Doom II). */
static const char * iwad_name_for (const uint8_t * p_wad)
{
    uint32_t num_lumps;
    uint32_t dir_offset;

    memcpy(&num_lumps, p_wad + 4, 4);
    memcpy(&dir_offset, p_wad + 8, 4);

    const uint8_t * p_dir     = p_wad + dir_offset;
    bool            has_map01 = false;
    bool            has_e2m1  = false;

    for (uint32_t i = 0; i < num_lumps; i++)
    {
        const char * p_name = (const char *) (p_dir + (i * 16U) + 8U);

        if (0 == strncmp(p_name, "MAP01", 8))
        {
            has_map01 = true;
        }
        else if (0 == strncmp(p_name, "E2M1", 8))
        {
            has_e2m1 = true;
        }
        else
        {
            /* Not interesting. */
        }
    }

    if (has_map01)
    {
        return "doom2.wad";
    }

    return has_e2m1 ? "doom.wad" : "doom1.wad";
}

static void halt_with_message (const char * p_line1, const char * p_line2)
{
    printf("%s - %s\n", p_line1, p_line2);
    ra8_video_boot_message(p_line1, p_line2);

    for (;;)
    {
        R_IOPORT_PinWrite(&g_ioport_ctrl, USER_LED_RED, BSP_IO_LEVEL_HIGH);
        vTaskDelay(pdMS_TO_TICKS(250));
        R_IOPORT_PinWrite(&g_ioport_ctrl, USER_LED_RED, BSP_IO_LEVEL_LOW);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void new_thread0_entry (void * pvParameters)
{
    FSP_PARAMETER_NOT_USED(pvParameters);

    rtt_log_init();
    printf("\n\nRA8P1 DOOM - EK-RA8P1, Cortex-M85 @ %lu MHz\n",
           (unsigned long) (SystemCoreClock / 1000000U));

    ra8_video_early_init();
    ra8_video_boot_message("RA8P1 DOOM", "LOADING...");

    usb_gamepad_task_start();
    usb_audio_task_start();

    uint8_t * p_wad = NULL;
    size_t    size  = wad_loader_load(&p_wad);

    if (0U == size)
    {
        halt_with_message("NO IWAD IN OSPI FLASH", "RUN TASK: FLASH DOOM WAD TO OSPI");
    }

    const char * p_name = iwad_name_for(p_wad);
    ramfs_register_rom_file(p_name, p_wad, size);
    printf("WAD: registered as %s\n", p_name);

    R_IOPORT_PinWrite(&g_ioport_ctrl, USER_LED_GREEN, BSP_IO_LEVEL_HIGH);

    static char * argv[] =
    {
        "doom",
        "-iwad",
        NULL,                          /* Filled in below */
        "-nogui",
        NULL,
    };
    argv[2] = (char *) p_name;

    doomgeneric_Create(4, argv);

    for (;;)
    {
        doomgeneric_Tick();
    }
}
