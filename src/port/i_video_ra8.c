/*
 * Doom video back end for the EK-RA8P1 parallel RGB display (1024x600).
 *
 * The GLCDC graphics layer 1 runs in CLUT8 mode, which matches Doom's native
 * 8-bit paletted frame buffer exactly:
 *   - I_SetPalette() only rewrites the 256 entry hardware colour table
 *     (palette flashes for damage / pickups cost nothing).
 *   - I_FinishUpdate() scales the 320x200 frame into one of two SDRAM frame
 *     buffers and flips to it on the next vertical sync.
 *
 * Scaling: 320x200 -> 800x600 (4:3 aspect correct, 2.5x horizontally, 3x
 * vertically) or 960x600 (integer 3x), selected by RA8_ASPECT_CORRECT.
 */
#include <string.h>
#include <stdio.h>

#include "hal_data.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "ra8_port.h"

#include "doomtype.h"
#include "doomgeneric.h"
#include "i_video.h"
#include "tables.h"
#include "z_zone.h"

/* ------------------------------------------------------------------------- */
/* Geometry                                                                   */
/* ------------------------------------------------------------------------- */

#if RA8_ASPECT_CORRECT
 #define OUT_WIDTH      (800)
#else
 #define OUT_WIDTH      (960)
#endif
#define OUT_HEIGHT      (SCREENHEIGHT * 3)
#define OUT_X           ((RA8_LCD_WIDTH - OUT_WIDTH) / 2)
#define OUT_Y           ((RA8_LCD_HEIGHT - OUT_HEIGHT) / 2)
#define FB_STRIDE       (DISPLAY_BUFFER_STRIDE_BYTES_INPUT0)
#define FB_BYTES        (sizeof(fb_background[0]))

/* ------------------------------------------------------------------------- */
/* Globals required by the Doom engine (normally provided by i_video.c)       */
/* ------------------------------------------------------------------------- */

byte        *I_VideoBuffer      = NULL;
boolean      screensaver_mode   = false;
boolean      screenvisible      = true;
float        mouse_acceleration = 2.0f;
int          mouse_threshold    = 10;
int          usegamma           = 0;
int          usemouse           = 0;
struct color colors[256];

/* Doom renders into this buffer; keep it in internal SRAM for speed. */
static byte g_doom_screen[SCREENWIDTH * SCREENHEIGHT] BSP_ALIGN_VARIABLE(32);

/* ------------------------------------------------------------------------- */
/* Display state                                                              */
/* ------------------------------------------------------------------------- */

static SemaphoreHandle_t g_vsync_sem;
static StaticSemaphore_t g_vsync_sem_mem;
static volatile uint32_t g_vsync_count;

static int      g_front;              /* Index of the buffer being scanned out */
static uint32_t g_flip_vsync;         /* g_vsync_count when the last flip was queued */
static bool     g_display_running = false;
static bool     g_backlight_on    = false;

static uint32_t g_clut[256];          /* ARGB8888 hardware palette            */
static bool     g_clut_pending    = false;

/* Simple frame rate statistics, printed on the RTT console. */
static uint32_t g_fps_frames;
static uint32_t g_fps_start;

/* GLCDC interrupt: fires once per frame at the line detection point. */
void glcdc_callback (display_callback_args_t * p_args)
{
    if (DISPLAY_EVENT_LINE_DETECTION == p_args->event)
    {
        BaseType_t woken = pdFALSE;

        g_vsync_count++;
        xSemaphoreGiveFromISR(g_vsync_sem, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

static void wait_vsync (void)
{
    if (!g_display_running)
    {
        return;
    }

    /* Clear a stale give, then wait for the next frame (with a timeout so a
     * stalled display never hangs the game). */
    (void) xSemaphoreTake(g_vsync_sem, 0);
    (void) xSemaphoreTake(g_vsync_sem, pdMS_TO_TICKS(50));
}

static void apply_clut (void)
{
    display_clut_cfg_t clut_cfg =
    {
        .p_base = g_clut,
        .start  = 0,
        .size   = 256,
    };

    if (FSP_SUCCESS == R_GLCDC_ClutUpdate(&g_display0_ctrl, &clut_cfg, DISPLAY_FRAME_LAYER_1))
    {
        g_clut_pending = false;
    }

    /* Otherwise the previous update has not been latched yet: retry on the
     * next frame. */
}

static void flip_to (int buffer)
{
    for (int retry = 0; retry < 3; retry++)
    {
        fsp_err_t err = R_GLCDC_BufferChange(&g_display0_ctrl, fb_background[buffer], DISPLAY_FRAME_LAYER_1);
        if (FSP_SUCCESS == err)
        {
            break;
        }

        wait_vsync();                  /* Register update still in progress */
    }

    g_flip_vsync = g_vsync_count;
    g_front      = buffer;
}

/* ------------------------------------------------------------------------- */
/* Scaling                                                                    */
/* ------------------------------------------------------------------------- */

static uint8_t g_line[OUT_WIDTH] BSP_ALIGN_VARIABLE(32);

static void scale_line (uint8_t * p_dst, const uint8_t * p_src)
{
#if RA8_ASPECT_CORRECT
    /* 2 source pixels -> 5 output pixels (pattern 3,2). */
    for (int x = 0; x < SCREENWIDTH; x += 2)
    {
        uint8_t a = p_src[x];
        uint8_t b = p_src[x + 1];

        p_dst[0] = a;
        p_dst[1] = a;
        p_dst[2] = a;
        p_dst[3] = b;
        p_dst[4] = b;
        p_dst   += 5;
    }
#else
    for (int x = 0; x < SCREENWIDTH; x++)
    {
        uint8_t a = p_src[x];

        p_dst[0] = a;
        p_dst[1] = a;
        p_dst[2] = a;
        p_dst   += 3;
    }
#endif
}

static void blit_frame (uint8_t * p_fb)
{
    const uint8_t * p_src = I_VideoBuffer;
    uint8_t       * p_row = p_fb + (OUT_Y * FB_STRIDE) + OUT_X;

    for (int y = 0; y < SCREENHEIGHT; y++)
    {
        scale_line(g_line, p_src);

        memcpy(p_row, g_line, OUT_WIDTH);
        memcpy(p_row + FB_STRIDE, g_line, OUT_WIDTH);
        memcpy(p_row + (2 * FB_STRIDE), g_line, OUT_WIDTH);

        p_src += SCREENWIDTH;
        p_row += 3 * FB_STRIDE;
    }
}

/* ------------------------------------------------------------------------- */
/* Boot screen (drawn before any Doom data is available)                      */
/* ------------------------------------------------------------------------- */

/* Classic 5x7 font, ASCII 0x20..0x5F, column major, LSB = top row. */
static const uint8_t g_font5x7[64][5] =
{
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x56,0x20,0x50},{0x00,0x08,0x07,0x03,0x00},
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x2A,0x1C,0x7F,0x1C,0x2A},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x80,0x70,0x30,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x00,0x60,0x60,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x72,0x49,0x49,0x49,0x46},{0x21,0x41,0x49,0x4D,0x33},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x31},{0x41,0x21,0x11,0x09,0x07},
    {0x36,0x49,0x49,0x49,0x36},{0x46,0x49,0x49,0x29,0x1E},{0x00,0x00,0x14,0x00,0x00},{0x00,0x40,0x34,0x00,0x00},
    {0x00,0x08,0x14,0x22,0x41},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x59,0x09,0x06},
    {0x3E,0x41,0x5D,0x59,0x4E},{0x7C,0x12,0x11,0x12,0x7C},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x41,0x3E},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x41,0x51,0x73},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x1C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x26,0x49,0x49,0x49,0x32},
    {0x03,0x01,0x7F,0x01,0x03},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63},{0x03,0x04,0x78,0x04,0x03},{0x61,0x59,0x49,0x4D,0x43},{0x00,0x7F,0x41,0x41,0x41},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x41,0x7F},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
};

/* Boot palette indices (only used until Doom loads PLAYPAL). */
enum { BOOT_BLACK = 0, BOOT_RED = 1, BOOT_WHITE = 2, BOOT_GREY = 3 };

static void draw_text (uint8_t * p_fb, int x0, int y0, int scale, uint8_t colour, const char * p_text)
{
    for (; *p_text; p_text++, x0 += 6 * scale)
    {
        int c = (unsigned char) *p_text;

        if ((c >= 'a') && (c <= 'z'))
        {
            c -= 'a' - 'A';
        }

        if ((c < 0x20) || (c > 0x5F))
        {
            c = '?';
        }

        const uint8_t * p_glyph = g_font5x7[c - 0x20];

        for (int col = 0; col < 5; col++)
        {
            for (int row = 0; row < 8; row++)
            {
                if (0 == (p_glyph[col] & (1U << row)))
                {
                    continue;
                }

                for (int sy = 0; sy < scale; sy++)
                {
                    int py = y0 + (row * scale) + sy;
                    int px = x0 + (col * scale);

                    if ((py < 0) || (py >= RA8_LCD_HEIGHT) || (px < 0) || ((px + scale) > RA8_LCD_WIDTH))
                    {
                        continue;
                    }

                    memset(p_fb + (py * FB_STRIDE) + px, colour, (size_t) scale);
                }
            }
        }
    }
}

static void draw_centred (uint8_t * p_fb, int y, int scale, uint8_t colour, const char * p_text)
{
    int width = (int) strlen(p_text) * 6 * scale;

    draw_text(p_fb, (RA8_LCD_WIDTH - width) / 2, y, scale, colour, p_text);
}

static void backlight_on (void)
{
    if (!g_backlight_on)
    {
        R_IOPORT_PinWrite(&g_ioport_ctrl, DISP_BLEN, BSP_IO_LEVEL_HIGH);
        g_backlight_on = true;
    }
}

void ra8_video_early_init (void)
{
    if (g_display_running)
    {
        return;
    }

    g_vsync_sem = xSemaphoreCreateBinaryStatic(&g_vsync_sem_mem);

    /* Panel reset (shared with the touch controller): high-low-high. */
    R_IOPORT_PinWrite(&g_ioport_ctrl, DISP_RESET, BSP_IO_LEVEL_HIGH);
    R_BSP_SoftwareDelay(1, BSP_DELAY_UNITS_MILLISECONDS);
    R_IOPORT_PinWrite(&g_ioport_ctrl, DISP_RESET, BSP_IO_LEVEL_LOW);
    R_BSP_SoftwareDelay(1, BSP_DELAY_UNITS_MILLISECONDS);
    R_IOPORT_PinWrite(&g_ioport_ctrl, DISP_RESET, BSP_IO_LEVEL_HIGH);
    R_BSP_SoftwareDelay(1, BSP_DELAY_UNITS_MILLISECONDS);

    memset(fb_background[0], BOOT_BLACK, FB_BYTES);
    memset(fb_background[1], BOOT_BLACK, FB_BYTES);

    /* Boot palette. */
    memset(g_clut, 0, sizeof(g_clut));
    g_clut[BOOT_BLACK] = 0xFF000000U;
    g_clut[BOOT_RED]   = 0xFFC00000U;
    g_clut[BOOT_WHITE] = 0xFFFFFFFFU;
    g_clut[BOOT_GREY]  = 0xFF808080U;

    fsp_err_t err = R_GLCDC_Open(&g_display0_ctrl, &g_display0_cfg);
    if (FSP_SUCCESS != err)
    {
        printf("VIDEO: R_GLCDC_Open failed (%d)\n", (int) err);

        return;
    }

    /* Load the boot palette into both CLUT planes before scan out starts. */
    for (int plane = 0; plane < 2; plane++)
    {
        display_clut_cfg_t clut_cfg = { .p_base = g_clut, .start = 0, .size = 256 };
        (void) R_GLCDC_ClutUpdate(&g_display0_ctrl, &clut_cfg, DISPLAY_FRAME_LAYER_1);
    }

    err = R_GLCDC_Start(&g_display0_ctrl);
    if (FSP_SUCCESS != err)
    {
        printf("VIDEO: R_GLCDC_Start failed (%d)\n", (int) err);

        return;
    }

    g_display_running = true;
    g_front           = 0;

    /* Show one blank frame before enabling the backlight (avoids a flash of
     * uninitialised panel content). */
    wait_vsync();
    wait_vsync();
    backlight_on();

    printf("VIDEO: GLCDC running, %dx%d CLUT8, game area %dx%d at (%d,%d)\n",
           RA8_LCD_WIDTH, RA8_LCD_HEIGHT, OUT_WIDTH, OUT_HEIGHT, OUT_X, OUT_Y);
}

void ra8_video_boot_message (const char * p_line1, const char * p_line2)
{
    if (!g_display_running)
    {
        return;
    }

    /* Draw into the front buffer directly; this is only used before Doom
     * owns the display (or when it has stopped because of an error). */
    uint8_t * p_fb = fb_background[g_front];

    memset(p_fb + (200 * FB_STRIDE), BOOT_BLACK, 200U * FB_STRIDE);

    if (NULL != p_line1)
    {
        draw_centred(p_fb, 220, 5, BOOT_RED, p_line1);
    }

    if (NULL != p_line2)
    {
        draw_centred(p_fb, 300, 3, BOOT_WHITE, p_line2);
    }

    /* Restore a palette in which the boot colours are visible. */
    g_clut[BOOT_BLACK] = 0xFF000000U;
    g_clut[BOOT_RED]   = 0xFFC00000U;
    g_clut[BOOT_WHITE] = 0xFFFFFFFFU;
    g_clut[BOOT_GREY]  = 0xFF808080U;
    wait_vsync();
    apply_clut();
}

/* ------------------------------------------------------------------------- */
/* Doom video interface (i_video.h)                                           */
/* ------------------------------------------------------------------------- */

void I_InitGraphics (void)
{
    ra8_video_early_init();

    I_VideoBuffer = g_doom_screen;
    memset(I_VideoBuffer, 0, sizeof(g_doom_screen));

    /* Clear both frame buffers: palette index 0 is black in PLAYPAL. */
    memset(fb_background[0], 0, FB_BYTES);
    memset(fb_background[1], 0, FB_BYTES);

    screenvisible = true;

    g_fps_start  = xTaskGetTickCount();
    g_fps_frames = 0;

    extern void I_InitInput(void);
    I_InitInput();
}

void I_ShutdownGraphics (void)
{
}

void I_StartFrame (void)
{
}

void I_StartTic (void)
{
    extern void I_GetEvent(void);
    I_GetEvent();
}

void I_UpdateNoBlit (void)
{
}

void I_FinishUpdate (void)
{
    int back = g_front ^ 1;

    /* The back buffer is still being scanned out until one vertical sync has
     * passed since the previous flip was queued. */
    if (g_display_running && (g_vsync_count == g_flip_vsync))
    {
        wait_vsync();
    }

    blit_frame(fb_background[back]);

    if (g_clut_pending)
    {
        apply_clut();
    }

    if (g_display_running)
    {
        flip_to(back);
    }

    /* Frame rate report every 5 seconds. */
    g_fps_frames++;
    uint32_t now = xTaskGetTickCount();
    if ((now - g_fps_start) >= pdMS_TO_TICKS(5000))
    {
        uint32_t ms = (now - g_fps_start) * portTICK_PERIOD_MS;
        printf("VIDEO: %lu.%lu fps\n", (unsigned long) ((g_fps_frames * 1000U) / ms),
               (unsigned long) (((g_fps_frames * 10000U) / ms) % 10U));
        g_fps_frames = 0;
        g_fps_start  = now;
    }
}

void I_ReadScreen (byte * scr)
{
    memcpy(scr, I_VideoBuffer, SCREENWIDTH * SCREENHEIGHT);
}

void I_SetPalette (byte * palette)
{
    for (int i = 0; i < 256; i++)
    {
        uint32_t r = gammatable[usegamma][*palette++];
        uint32_t g = gammatable[usegamma][*palette++];
        uint32_t b = gammatable[usegamma][*palette++];

        colors[i].a = 0xFF;
        colors[i].r = r & 0xFFU;
        colors[i].g = g & 0xFFU;
        colors[i].b = b & 0xFFU;

        g_clut[i] = 0xFF000000U | (r << 16) | (g << 8) | b;
    }

    g_clut_pending = true;
}

int I_GetPaletteIndex (int r, int g, int b)
{
    int best      = 0;
    int best_diff = 0x7FFFFFFF;

    for (int i = 0; i < 256; ++i)
    {
        int dr   = r - (int) colors[i].r;
        int dg   = g - (int) colors[i].g;
        int db   = b - (int) colors[i].b;
        int diff = (dr * dr) + (dg * dg) + (db * db);

        if (diff < best_diff)
        {
            best      = i;
            best_diff = diff;
        }

        if (0 == diff)
        {
            break;
        }
    }

    return best;
}

void I_BeginRead (void)
{
}

void I_EndRead (void)
{
}

void I_SetWindowTitle (char * title)
{
    (void) title;
}

void I_GraphicsCheckCommandLine (void)
{
}

void I_SetGrabMouseCallback (grabmouse_callback_t func)
{
    (void) func;
}

void I_EnableLoadingDisk (void)
{
}

void I_BindVideoVariables (void)
{
}

void I_DisplayFPSDots (boolean dots_on)
{
    (void) dots_on;
}

void I_CheckIsScreensaver (void)
{
}
