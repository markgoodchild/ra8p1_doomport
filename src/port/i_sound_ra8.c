/*
 * Doom sound effects for the EK-RA8P1: doomgeneric DG_sound_module.
 *
 * Doom's sound effects are 8-bit unsigned mono DMX lumps (normally 11025 Hz).
 * They are played in place from the memory-mapped WAD in SDRAM, resampled
 * (linear interpolation) to the USB output rate, panned and mixed to 16-bit
 * stereo by audio_mixer_render(), which the USB audio task calls whenever it
 * needs another block.
 *
 * The Doom thread only updates the channel table inside short critical
 * sections; the mixer takes a snapshot, mixes without holding the lock and
 * then writes the play positions back for the channels that weren't
 * restarted meanwhile (tracked with a per-channel generation count).
 *
 * Music (MUS -> OPL FM synthesis) is not implemented; DG_music_module is a
 * silent stub.
 */
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "ra8_port.h"

#include "doomtype.h"
#include "i_sound.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"

#define MIX_CHANNELS        (16)       /* >= snd_channels (8 by default) */
#define DMX_HEADER_BYTES    (8U)
#define DMX_PAD_BYTES       (16U)      /* DMX pads each end of the sample data */

typedef struct
{
    const uint8_t * p_data;            /* 8-bit unsigned samples        */
    uint32_t        length;            /* Number of samples              */
    uint32_t        pos;               /* 16.16 fixed point position     */
    uint32_t        step;              /* 16.16 increment per out sample */
    int32_t         gain_l;            /* 0..255                         */
    int32_t         gain_r;
    uint32_t        generation;        /* Bumped on every start / stop   */
    bool            active;
} mix_channel_t;

static mix_channel_t     g_channels[MIX_CHANNELS];
static volatile uint32_t g_output_rate = 48000U;
static boolean           g_use_sfx_prefix;

/* Referenced by m_config.c / i_sound.c when FEATURE_SOUND is defined (they
 * belong to the SDL sound module, which isn't used here). */
int   use_libsamplerate   = 0;
float libsamplerate_scale = 0.65f;

void I_InitTimidityConfig (void)
{
}

/* ------------------------------------------------------------------------- */
/* Mixer (USB audio task)                                                     */
/* ------------------------------------------------------------------------- */

void audio_mixer_set_output_rate (uint32_t rate)
{
    g_output_rate = rate;
}

void audio_mixer_render (int16_t * p_stereo, uint32_t frames)
{
    mix_channel_t snap[MIX_CHANNELS];
    static int32_t s_acc[2 * 48 * 16]; /* Up to 16 ms at 48 kHz */

    if (frames > (sizeof(s_acc) / sizeof(s_acc[0]) / 2U))
    {
        frames = (uint32_t) (sizeof(s_acc) / sizeof(s_acc[0]) / 2U);
    }

    taskENTER_CRITICAL();
    memcpy(snap, g_channels, sizeof(snap));
    taskEXIT_CRITICAL();

    memset(s_acc, 0, frames * 2U * sizeof(int32_t));

    for (uint32_t c = 0; c < MIX_CHANNELS; c++)
    {
        mix_channel_t * p_ch = &snap[c];

        if (!p_ch->active)
        {
            continue;
        }

        const uint8_t * p_data = p_ch->p_data;
        uint32_t        end    = p_ch->length << 16;
        uint32_t        pos    = p_ch->pos;
        uint32_t        step   = p_ch->step;
        int32_t         gl     = p_ch->gain_l;
        int32_t         gr     = p_ch->gain_r;

        for (uint32_t i = 0; i < frames; i++)
        {
            if (pos >= end)
            {
                p_ch->active = false;
                break;
            }

            uint32_t idx  = pos >> 16;
            int32_t  frac = (int32_t) (pos & 0xFFFFU);
            int32_t  s0   = (int32_t) p_data[idx] - 128;
            int32_t  s1   = ((idx + 1U) < p_ch->length) ? ((int32_t) p_data[idx + 1U] - 128) : s0;
            int32_t  s    = (s0 << 8) + (((s1 - s0) * frac) >> 8);   /* 16-bit range */

            s_acc[(2U * i)]      += (s * gl) >> 8;
            s_acc[(2U * i) + 1U] += (s * gr) >> 8;

            pos += step;
        }

        p_ch->pos = pos;
    }

    for (uint32_t i = 0; i < (frames * 2U); i++)
    {
        int32_t s = s_acc[i];

        if (s > 32767)
        {
            s = 32767;
        }
        else if (s < -32768)
        {
            s = -32768;
        }
        else
        {
            /* In range */
        }

        p_stereo[i] = (int16_t) s;
    }

    /* Write back progress unless Doom restarted / stopped the channel. */
    taskENTER_CRITICAL();
    for (uint32_t c = 0; c < MIX_CHANNELS; c++)
    {
        if (g_channels[c].active && (g_channels[c].generation == snap[c].generation))
        {
            g_channels[c].pos    = snap[c].pos;
            g_channels[c].active = snap[c].active;
        }
    }

    taskEXIT_CRITICAL();
}

/* ------------------------------------------------------------------------- */
/* Doom sound module (Doom thread)                                            */
/* ------------------------------------------------------------------------- */

static snddevice_t g_sound_devices[] =
{
    SNDDEVICE_SB,
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_AWE32,
};

/* Doom passes vol 0..127 (already scaled by the SFX volume) and sep 0..254
 * (128 = centre); same law as Chocolate Doom's SDL module. */
static void calc_gains (int vol, int sep, int32_t * p_left, int32_t * p_right)
{
    int32_t left  = ((254 - sep) * vol) / 127;
    int32_t right = (sep * vol) / 127;

    *p_left  = (left < 0) ? 0 : ((left > 255) ? 255 : left);
    *p_right = (right < 0) ? 0 : ((right > 255) ? 255 : right);
}

static boolean snd_init (boolean use_sfx_prefix)
{
    g_use_sfx_prefix = use_sfx_prefix;
    memset(g_channels, 0, sizeof(g_channels));
    printf("SOUND: software mixer ready, %d channels, output to USB audio\n", MIX_CHANNELS);

    return true;
}

static void snd_shutdown (void)
{
    taskENTER_CRITICAL();
    for (uint32_t c = 0; c < MIX_CHANNELS; c++)
    {
        g_channels[c].active = false;
        g_channels[c].generation++;
    }

    taskEXIT_CRITICAL();
}

static int snd_get_sfx_lump_num (sfxinfo_t * sfx)
{
    char name[9];

    if (NULL != sfx->link)
    {
        sfx = sfx->link;
    }

    if (g_use_sfx_prefix)
    {
        M_snprintf(name, sizeof(name), "ds%s", sfx->name);
    }
    else
    {
        M_snprintf(name, sizeof(name), "%s", sfx->name);
    }

    return W_CheckNumForName(name);
}

static void snd_update (void)
{
}

static void snd_update_params (int channel, int vol, int sep)
{
    int32_t gl;
    int32_t gr;

    if ((channel < 0) || (channel >= MIX_CHANNELS))
    {
        return;
    }

    calc_gains(vol, sep, &gl, &gr);

    taskENTER_CRITICAL();
    g_channels[channel].gain_l = gl;
    g_channels[channel].gain_r = gr;
    taskEXIT_CRITICAL();
}

static int snd_start_sound (sfxinfo_t * sfxinfo, int channel, int vol, int sep)
{
    if ((channel < 0) || (channel >= MIX_CHANNELS) || (sfxinfo->lumpnum < 0))
    {
        return -1;
    }

    const uint8_t * p_lump = sfxinfo->driver_data;
    if (NULL == p_lump)
    {
        p_lump               = W_CacheLumpNum(sfxinfo->lumpnum, PU_STATIC);
        sfxinfo->driver_data = (void *) p_lump;
    }

    uint32_t lump_len = (uint32_t) W_LumpLength((unsigned int) sfxinfo->lumpnum);
    uint32_t format   = (uint32_t) p_lump[0] | ((uint32_t) p_lump[1] << 8);
    uint32_t rate     = (uint32_t) p_lump[2] | ((uint32_t) p_lump[3] << 8);
    uint32_t length   = (uint32_t) p_lump[4] | ((uint32_t) p_lump[5] << 8) |
                        ((uint32_t) p_lump[6] << 16) | ((uint32_t) p_lump[7] << 24);

    /* Same validity checks as Chocolate Doom. */
    if ((lump_len < DMX_HEADER_BYTES) || (3U != format) || (0U == rate) ||
        (length > (lump_len - DMX_HEADER_BYTES)) || (length <= (2U * DMX_PAD_BYTES) + 16U))
    {
        return -1;
    }

    int32_t gl;
    int32_t gr;
    calc_gains(vol, sep, &gl, &gr);

    uint32_t out_rate = g_output_rate;

    taskENTER_CRITICAL();
    mix_channel_t * p_ch = &g_channels[channel];
    p_ch->p_data = p_lump + DMX_HEADER_BYTES + DMX_PAD_BYTES;
    p_ch->length = length - (2U * DMX_PAD_BYTES);
    p_ch->pos    = 0;
    p_ch->step   = (uint32_t) (((uint64_t) rate << 16) / out_rate);
    p_ch->gain_l = gl;
    p_ch->gain_r = gr;
    p_ch->generation++;
    p_ch->active = true;
    taskEXIT_CRITICAL();

    return channel;
}

static void snd_stop_sound (int channel)
{
    if ((channel < 0) || (channel >= MIX_CHANNELS))
    {
        return;
    }

    taskENTER_CRITICAL();
    g_channels[channel].active = false;
    g_channels[channel].generation++;
    taskEXIT_CRITICAL();
}

static boolean snd_sound_is_playing (int channel)
{
    if ((channel < 0) || (channel >= MIX_CHANNELS))
    {
        return false;
    }

    return g_channels[channel].active;
}

static void snd_cache_sounds (sfxinfo_t * sounds, int num_sounds)
{
    /* Lumps are memory mapped from SDRAM, so caching is just a pointer. */
    for (int i = 0; i < num_sounds; i++)
    {
        sfxinfo_t * p_sfx = &sounds[i];
        int         lump  = snd_get_sfx_lump_num(p_sfx);

        p_sfx->lumpnum = lump;
        if ((lump >= 0) && (NULL == p_sfx->driver_data))
        {
            p_sfx->driver_data = W_CacheLumpNum(lump, PU_STATIC);
        }
    }
}

sound_module_t DG_sound_module =
{
    g_sound_devices,
    (int) (sizeof(g_sound_devices) / sizeof(g_sound_devices[0])),
    snd_init,
    snd_shutdown,
    snd_get_sfx_lump_num,
    snd_update,
    snd_update_params,
    snd_start_sound,
    snd_stop_sound,
    snd_sound_is_playing,
    snd_cache_sounds,
};

/* ------------------------------------------------------------------------- */
/* Music: not implemented (silent stub)                                       */
/* ------------------------------------------------------------------------- */

static boolean mus_init (void)
{
    return true;
}

static void mus_shutdown (void)
{
}

static void mus_set_volume (int volume)
{
    (void) volume;
}

static void mus_pause (void)
{
}

static void mus_resume (void)
{
}

static void * mus_register_song (void * data, int len)
{
    (void) data;
    (void) len;

    return NULL;
}

static void mus_unregister_song (void * handle)
{
    (void) handle;
}

static void mus_play_song (void * handle, boolean looping)
{
    (void) handle;
    (void) looping;
}

static void mus_stop_song (void)
{
}

static boolean mus_is_playing (void)
{
    return false;
}

music_module_t DG_music_module =
{
    NULL,
    0,
    mus_init,
    mus_shutdown,
    mus_set_volume,
    mus_pause,
    mus_resume,
    mus_register_song,
    mus_unregister_song,
    mus_play_song,
    mus_stop_song,
    mus_is_playing,
    NULL,
};
