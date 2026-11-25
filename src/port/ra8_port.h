/*
 * RA8P1 Doom port - shared definitions between the platform layer modules.
 *
 * Hardware: EK-RA8P1 (R7KA8P1KF, Cortex-M85 @ 1 GHz)
 *   - Display : RTKLCDPAR1S00001BE 7" 1024x600 parallel RGB panel (GLCDC, CLUT8)
 *   - Input   : Sony DualSense (PS5) / DualShock 4 on the USB high-speed port
 *   - Audio   : USB audio class headphones / dongle on the USB full-speed port
 *   - Storage : IWAD in the external octal flash (OSPI0 CS1, 0x90000000),
 *               copied to SDRAM at boot
 */
#ifndef RA8_PORT_H
#define RA8_PORT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ------------------------------------------------------------------------- */
/* Memory layout                                                              */
/* ------------------------------------------------------------------------- */

/* Base address where the IWAD is programmed in the external OSPI flash
 * (see scripts/flash_wad.bat). */
#define RA8_WAD_OSPI_ADDRESS     (0x90000000U)

/* Largest IWAD supported (DOOM2.WAD is 14.6 MB). */
#define RA8_WAD_MAX_BYTES        (16U * 1024U * 1024U)

/* Doom zone allocator size (vanilla needs ~4 MB, chocolate defaults to 16). */
#define RA8_ZONE_BYTES           (24U * 1024U * 1024U)

/* RAM file system pool, used for default.cfg and save games. */
#define RA8_RAMFS_POOL_BYTES     (2U * 1024U * 1024U)

/* ------------------------------------------------------------------------- */
/* Display                                                                    */
/* ------------------------------------------------------------------------- */

#define RA8_LCD_WIDTH            (1024)
#define RA8_LCD_HEIGHT           (600)

/* 1 = 4:3 aspect correct output (320x200 -> 800x600, horizontal scale 2.5x).
 * 0 = integer scaling (320x200 -> 960x600, 3x in both directions). */
#ifndef RA8_ASPECT_CORRECT
 #define RA8_ASPECT_CORRECT      (1)
#endif

/* ------------------------------------------------------------------------- */
/* Game pad state, produced by the USB task, consumed by the Doom thread      */
/* ------------------------------------------------------------------------- */

/* Logical buttons, independent of the controller model. */
enum
{
    PAD_BTN_CROSS    = (1U << 0),
    PAD_BTN_CIRCLE   = (1U << 1),
    PAD_BTN_SQUARE   = (1U << 2),
    PAD_BTN_TRIANGLE = (1U << 3),
    PAD_BTN_L1       = (1U << 4),
    PAD_BTN_R1       = (1U << 5),
    PAD_BTN_L2       = (1U << 6),
    PAD_BTN_R2       = (1U << 7),
    PAD_BTN_CREATE   = (1U << 8),  /* "Share" on DualShock 4 */
    PAD_BTN_OPTIONS  = (1U << 9),
    PAD_BTN_L3       = (1U << 10),
    PAD_BTN_R3       = (1U << 11),
    PAD_BTN_PS       = (1U << 12),
    PAD_BTN_TOUCH    = (1U << 13),
    PAD_BTN_UP       = (1U << 14),
    PAD_BTN_DOWN     = (1U << 15),
    PAD_BTN_LEFT     = (1U << 16),
    PAD_BTN_RIGHT    = (1U << 17),
};

typedef struct
{
    bool     connected;
    uint32_t buttons;            /* PAD_BTN_* bit mask                      */
    uint8_t  lx, ly, rx, ry;     /* Sticks, 0..255, 128 = centre            */
    uint8_t  l2, r2;             /* Analog triggers, 0..255                 */
    uint32_t report_count;       /* Incremented for every input report      */
} pad_state_t;

/* USB host task: enumerates the controller and keeps the state updated. */
void usb_gamepad_task_start(void);

/* Thread safe snapshot of the current controller state. */
void usb_gamepad_get_state(pad_state_t * p_state);

/* True once the game pad task has opened the USBHS host instance. */
bool usb_gamepad_host_ready(void);

/* ------------------------------------------------------------------------- */
/* Audio: USB headphones on the full-speed port, fed by the Doom SFX mixer    */
/* ------------------------------------------------------------------------- */

/* USB host audio task: enumerates the headphones and streams the mixer. */
void usb_audio_task_start(void);

/* True while audio is being streamed to a USB audio device. */
bool usb_audio_is_streaming(void);

/* Mixer (i_sound_ra8.c): set the output sample rate before rendering. */
void audio_mixer_set_output_rate(uint32_t rate);

/* Mixer: render `frames` interleaved stereo 16-bit samples. Called from the
 * USB audio task; safe against concurrent Doom sound calls. */
void audio_mixer_render(int16_t * p_stereo, uint32_t frames);

/* ------------------------------------------------------------------------- */
/* Boot services                                                              */
/* ------------------------------------------------------------------------- */

/* Initialise the OSPI flash and copy the IWAD into SDRAM.
 * Returns the number of bytes loaded, or 0 if no valid IWAD was found. */
size_t wad_loader_load(uint8_t ** pp_wad);

/* Register a read-only file image with the RAM file system. */
void ramfs_register_rom_file(const char * p_name, const uint8_t * p_data, size_t size);

/* Look up a RAM file system entry by name; returns NULL if not found. */
const uint8_t * ramfs_get_file_data(const char * p_name, size_t * p_size);

/* Simple SDRAM bump allocator for large, never-freed blocks. */
void * sdram_alloc(size_t size);

/* Video: open the GLCDC and show the boot screen. */
void ra8_video_early_init(void);

/* Draw a status/error message on the boot screen (no Doom assets needed). */
void ra8_video_boot_message(const char * p_line1, const char * p_line2);

/* Debug log output (SEGGER RTT channel 0). */
void rtt_log_init(void);
void rtt_log_write(const char * p_data, size_t len);
const char * rtt_log_last_line(void);

#endif /* RA8_PORT_H */
