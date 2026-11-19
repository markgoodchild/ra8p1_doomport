/*
 * IWAD loader: maps the external octal flash (OSPI0, chip select 1) in plain
 * SPI (1S-1S-1S) mode and copies the IWAD into SDRAM.
 *
 * The IWAD is programmed at 0x90000000 by scripts/flash_wad.bat (J-Link).
 * Keeping it out of the application image means a normal debug download only
 * rewrites the 1 MB internal MRAM, which is much faster than reprogramming
 * 4 MB of OSPI flash each time.
 */
#include <stdio.h>
#include <string.h>

#include "hal_data.h"
#include "FreeRTOS.h"
#include "task.h"
#include "ra8_port.h"

typedef struct
{
    char     id[4];                    /* "IWAD" */
    uint32_t num_lumps;
    uint32_t dir_offset;
} wad_header_t;

/* Pulse the flash reset line so that the device is back in its power-on SPI
 * mode, even if an earlier application switched it to octal DDR. */
static void ospi_flash_reset (void)
{
    g_ospi0_ctrl.p_reg->LIOCTL_b.RSTCS0 = 1;
    R_BSP_SoftwareDelay(1, BSP_DELAY_UNITS_MILLISECONDS);
    g_ospi0_ctrl.p_reg->LIOCTL_b.RSTCS0 = 0;
    R_BSP_SoftwareDelay(1, BSP_DELAY_UNITS_MILLISECONDS);
    g_ospi0_ctrl.p_reg->LIOCTL_b.RSTCS0 = 1;
    R_BSP_SoftwareDelay(1, BSP_DELAY_UNITS_MILLISECONDS);
}

size_t wad_loader_load (uint8_t ** pp_wad)
{
    *pp_wad = NULL;

    fsp_err_t err = R_OSPI_B_Open(&g_ospi0_ctrl, &g_ospi0_cfg);
    if (FSP_SUCCESS != err)
    {
        printf("WAD: R_OSPI_B_Open failed (%d)\n", (int) err);

        return 0;
    }

    ospi_flash_reset();

    const uint8_t * p_flash = (const uint8_t *) RA8_WAD_OSPI_ADDRESS;

    /* Discard any stale lines from the cache before reading the header. */
    SCB_InvalidateDCache_by_Addr((void *) (uintptr_t) p_flash, 64);

    wad_header_t header;
    memcpy(&header, p_flash, sizeof(header));

    if (0 != memcmp(header.id, "IWAD", 4))
    {
        printf("WAD: no IWAD at 0x%08lx (found %02x %02x %02x %02x)\n",
               (unsigned long) RA8_WAD_OSPI_ADDRESS,
               p_flash[0], p_flash[1], p_flash[2], p_flash[3]);

        return 0;
    }

    /* The directory is the last item in an IWAD, so it marks the file end. */
    uint64_t size = (uint64_t) header.dir_offset + ((uint64_t) header.num_lumps * 16U);

    if ((header.num_lumps == 0U) || (size > RA8_WAD_MAX_BYTES))
    {
        printf("WAD: implausible header (lumps=%lu, dir=0x%lx)\n",
               (unsigned long) header.num_lumps, (unsigned long) header.dir_offset);

        return 0;
    }

    uint8_t * p_wad = sdram_alloc((size_t) size);
    if (NULL == p_wad)
    {
        printf("WAD: out of SDRAM\n");

        return 0;
    }

    uint32_t start = xTaskGetTickCount();

    SCB_InvalidateDCache_by_Addr((void *) (uintptr_t) p_flash, (int32_t) size);
    memcpy(p_wad, p_flash, (size_t) size);

    uint32_t elapsed = xTaskGetTickCount() - start;

    printf("WAD: %lu bytes, %lu lumps copied from OSPI to %p in %lu ms\n",
           (unsigned long) size, (unsigned long) header.num_lumps, (void *) p_wad,
           (unsigned long) elapsed);

    *pp_wad = p_wad;

    return (size_t) size;
}
