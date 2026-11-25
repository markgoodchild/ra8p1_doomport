/*
 * USB host audio output (USB-C headphones / USB audio dongles) on the
 * EK-RA8P1 USB full-speed port (USBFS, IP0).
 *
 * The FSP USB Host Audio class driver (r_usb_haud) enumerates the device;
 * this task then:
 *   1. Parses the configuration descriptor and picks an isochronous OUT
 *      alternate setting with PCM, 1-2 channels, 2-4 byte samples and a
 *      sample rate that gives a whole number of samples per 1 ms frame
 *      (48, 32, 24 or 16 kHz).
 *   2. Selects that alternate setting, sets the sample rate (USB Audio 1.0
 *      endpoint control or USB Audio 2.0 clock source) and unmutes the
 *      feature units.
 *   3. Streams the Doom mixer output with two ping-pong buffers. Each
 *      R_USB_Write() carries AUDIO_BLOCK_MS packets of exactly one frame's
 *      worth of samples.
 *
 * The descriptor is parsed here rather than with R_USB_HAUD_AlternateNumberGet()
 * / R_USB_HAUD_ClockSourceGet() because those FSP 6.5 helpers only read the
 * low byte of wTotalLength (headset descriptors are often > 255 bytes) and
 * the Audio 2.0 clock source lookup never returns its result.
 *
 * The FSP host driver sends wMaxPacketSize bytes per isochronous packet, so
 * the endpoint descriptor's wMaxPacketSize is patched to the exact per-frame
 * payload before R_USB_HAUD_PipeSet() configures the pipe. Sending less than
 * wMaxPacketSize is always allowed by the USB specification.
 *
 * Asynchronous endpoints' feedback is ignored: the host clocks out exactly
 * rate/1000 samples per frame and the device absorbs the small drift.
 */
#include <stdio.h>
#include <string.h>

#include "new_thread0.h"
#include "queue.h"
#include "r_usb_haud.h"

/* FSP USB internals, for the class check workaround in audio_task(). */
#include "r_usb_typedef.h"
#include "r_usb_extern.h"
#include "r_usb_haud_driver.h"

#include "ra8_port.h"

#define AUDIO_TASK_STACK_WORDS  (1024)
#define AUDIO_TASK_PRIORITY     (3)     /* Same as the FSP MGR task, below HCD (4); the
                                         * next packet must be queued within ~1 ms */
#define AUDIO_EVENT_QUEUE_LEN   (16)

#define AUDIO_BLOCK_MS          (8)     /* USB packets (frames) per R_USB_Write()    */
#define AUDIO_MAX_RATE_KHZ      (48)
#define AUDIO_MAX_PACKET        (256)   /* USBFS isochronous pipe buffer limit       */
#define AUDIO_BUF_BYTES         (AUDIO_MAX_PACKET * AUDIO_BLOCK_MS)
#define AUDIO_MAX_FRAMES        (AUDIO_MAX_RATE_KHZ * AUDIO_BLOCK_MS)

#define AUDIO_CTRL_TIMEOUT_MS   (1000)

/* USB Audio descriptor constants */
#define DESC_INTERFACE          (0x04U)
#define DESC_ENDPOINT           (0x05U)
#define DESC_CS_INTERFACE       (0x24U)
#define AUDIO_CLASS             (0x01U)
#define AUDIO_SUBCLASS_CONTROL  (0x01U)
#define AUDIO_SUBCLASS_STREAM   (0x02U)
#define AUDIO_PROTOCOL_UAC2     (0x20U)

#define AC_INPUT_TERMINAL       (0x02U)
#define AC_FEATURE_UNIT         (0x06U)
#define AC2_CLOCK_SOURCE        (0x0AU)
#define AS_GENERAL              (0x01U)
#define AS_FORMAT_TYPE          (0x02U)
#define FORMAT_TYPE_I           (0x01U)

#define REQ_SET_CUR             (0x0100U)  /* bRequest 0x01 in the high byte (FSP layout) */
#define REQ_GET_MAX             (0x8300U)
#define FU_MUTE_CONTROL         (0x01U)
#define FU_VOLUME_CONTROL       (0x02U)

#define MAX_FEATURE_UNITS       (4)
#define MAX_TERMINALS           (8)

/* Feature unit info and Audio 2.0 clock topology, from the AudioControl interface. */
typedef struct
{
    bool    uac2;
    uint8_t ac_iface;
    uint8_t num_fu;
    uint8_t fu_id[MAX_FEATURE_UNITS];
    uint8_t num_it;
    uint8_t it_id[MAX_TERMINALS];
    uint8_t it_clock[MAX_TERMINALS];
    uint8_t first_clock;
} ac_info_t;

/* One AudioStreaming alternate setting being parsed / the one selected. */
typedef struct
{
    uint8_t   iface;
    uint8_t   alt;
    bool      pcm;
    uint8_t   terminal_link;
    uint8_t   channels;
    uint8_t   subframe;
    uint8_t   bits;
    const uint8_t * p_format;          /* Audio 1.0 Type I format descriptor (rates) */
    uint8_t * p_ep;                    /* Isochronous OUT endpoint descriptor          */
} as_alt_t;

typedef struct
{
    as_alt_t alt;
    uint32_t rate;
    uint32_t packet;                   /* Bytes per 1 ms frame */
    int32_t  score;
} as_choice_t;

static QueueHandle_t g_aud_queue;
static StaticQueue_t g_aud_queue_mem;
static uint8_t       g_aud_queue_storage[AUDIO_EVENT_QUEUE_LEN * sizeof(usb_event_info_t)];

static StaticTask_t  g_aud_task_tcb;
static StackType_t   g_aud_task_stack[AUDIO_TASK_STACK_WORDS];

static uint8_t       g_ctrl_buf[8] BSP_ALIGN_VARIABLE(4);
static uint8_t       g_tx_buf[2][AUDIO_BUF_BYTES] BSP_ALIGN_VARIABLE(4);
static int16_t       g_mix_buf[AUDIO_MAX_FRAMES * 2];

static uint8_t       g_dev_addr;
static volatile bool g_streaming;
static as_choice_t   g_cfg;
static uint32_t      g_block_bytes;
static uint32_t      g_frames_per_block;
static uint8_t       g_next_buf;       /* Buffer to send on the next write complete */
static uint32_t      g_write_errors;

/* FSP USB event callback (RTOS mode) for the USBFS instance. The event
 * structure is owned by the driver, so it is copied into our own queue. */
void usb_audio_callback (usb_event_info_t * p_event_info, usb_hdl_t cur_task, usb_onoff_t usb_state)
{
    FSP_PARAMETER_NOT_USED(cur_task);
    FSP_PARAMETER_NOT_USED(usb_state);

    if (NULL == g_aud_queue)
    {
        return;
    }

    if (xPortIsInsideInterrupt())
    {
        BaseType_t woken = pdFALSE;
        (void) xQueueSendFromISR(g_aud_queue, p_event_info, &woken);
        portYIELD_FROM_ISR(woken);
    }
    else
    {
        (void) xQueueSend(g_aud_queue, p_event_info, 0);
    }
}

bool usb_audio_is_streaming (void)
{
    return g_streaming;
}

/* ------------------------------------------------------------------------- */
/* Descriptor parsing                                                         */
/* ------------------------------------------------------------------------- */

static uint32_t rd24 (const uint8_t * p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16);
}

static bool uac1_rate_supported (const uint8_t * p_format, uint32_t rate)
{
    uint8_t n = p_format[7];           /* bSamFreqType */

    if (0U == n)
    {
        /* Continuous range: tLowerSamFreq, tUpperSamFreq */
        if (p_format[0] < 14U)
        {
            return false;
        }

        return (rate >= rd24(&p_format[8])) && (rate <= rd24(&p_format[11]));
    }

    for (uint8_t i = 0; i < n; i++)
    {
        if ((uint32_t) (8U + (i * 3U) + 3U) > p_format[0])
        {
            break;
        }

        if (rd24(&p_format[8U + (i * 3U)]) == rate)
        {
            return true;
        }
    }

    return false;
}

/* Score a complete alternate setting and keep it if it beats the best so far. */
static void consider_alt (const as_alt_t * p_alt, bool uac2, as_choice_t * p_best)
{
    static const uint32_t s_rates[] = {48000U, 32000U, 24000U, 16000U};

    if ((NULL == p_alt->p_ep) || !p_alt->pcm || (0U == p_alt->alt))
    {
        return;
    }

    if ((p_alt->channels < 1U) || (p_alt->channels > 2U) || (p_alt->subframe < 2U) || (p_alt->subframe > 4U))
    {
        return;
    }

    uint16_t mps = (uint16_t) (((uint32_t) p_alt->p_ep[4] | ((uint32_t) p_alt->p_ep[5] << 8)) & 0x7FFU);

    for (uint32_t i = 0; i < (sizeof(s_rates) / sizeof(s_rates[0])); i++)
    {
        uint32_t rate = s_rates[i];

        if (uac2)
        {
            /* Rates live in the clock source (GET RANGE); 48 kHz is universal. */
            if (48000U != rate)
            {
                continue;
            }
        }
        else if ((NULL == p_alt->p_format) || !uac1_rate_supported(p_alt->p_format, rate))
        {
            continue;
        }

        uint32_t packet = (rate / 1000U) * p_alt->channels * p_alt->subframe;

        if ((packet > AUDIO_MAX_PACKET) || (packet > mps))
        {
            continue;
        }

        /* Prefer the highest rate, then stereo, then 16-bit samples. */
        int32_t score = (int32_t) (rate / 100U) + ((2U == p_alt->channels) ? 4 : 0) + ((2U == p_alt->subframe) ? 2 : 0);

        if (score > p_best->score)
        {
            p_best->alt    = *p_alt;
            p_best->rate   = rate;
            p_best->packet = packet;
            p_best->score  = score;
        }

        return;                        /* Highest usable rate for this alt found */
    }
}

static bool parse_config (uint8_t * p_cfg, ac_info_t * p_ac, as_choice_t * p_best)
{
    uint32_t total = (uint32_t) p_cfg[2] | ((uint32_t) p_cfg[3] << 8);
    uint32_t off   = 0;
    bool     in_ac = false;
    bool     in_as = false;
    as_alt_t cur;

    memset(p_ac, 0, sizeof(*p_ac));
    memset(&cur, 0, sizeof(cur));
    memset(p_best, 0, sizeof(*p_best));
    p_best->score = -1;

    while ((off + 2U) <= total)
    {
        uint8_t * d   = &p_cfg[off];
        uint8_t   len = d[0];

        if ((len < 2U) || ((off + len) > total))
        {
            break;
        }

        switch (d[1])
        {
            case DESC_INTERFACE:
            {
                if (in_as)
                {
                    consider_alt(&cur, p_ac->uac2, p_best);
                }

                in_ac = (AUDIO_CLASS == d[5]) && (AUDIO_SUBCLASS_CONTROL == d[6]);
                in_as = (AUDIO_CLASS == d[5]) && (AUDIO_SUBCLASS_STREAM == d[6]);

                if (in_ac)
                {
                    p_ac->ac_iface = d[2];
                    p_ac->uac2     = (AUDIO_PROTOCOL_UAC2 == d[7]);
                }

                memset(&cur, 0, sizeof(cur));
                cur.iface = d[2];
                cur.alt   = d[3];
                break;
            }

            case DESC_CS_INTERFACE:
            {
                if (in_ac)
                {
                    if ((AC_FEATURE_UNIT == d[2]) && (p_ac->num_fu < MAX_FEATURE_UNITS))
                    {
                        p_ac->fu_id[p_ac->num_fu++] = d[3];
                    }
                    else if (p_ac->uac2 && (AC_INPUT_TERMINAL == d[2]) && (len >= 8U) &&
                             (p_ac->num_it < MAX_TERMINALS))
                    {
                        p_ac->it_id[p_ac->num_it]    = d[3];
                        p_ac->it_clock[p_ac->num_it] = d[7];  /* bCSourceID */
                        p_ac->num_it++;
                    }
                    else if (p_ac->uac2 && (AC2_CLOCK_SOURCE == d[2]) && (0U == p_ac->first_clock))
                    {
                        p_ac->first_clock = d[3];
                    }
                    else
                    {
                        /* Not needed. */
                    }
                }
                else if (in_as)
                {
                    if (AS_GENERAL == d[2])
                    {
                        cur.terminal_link = d[3];

                        if (p_ac->uac2)
                        {
                            /* bFormatType = I, bmFormats bit 0 = PCM, bNrChannels */
                            cur.pcm      = (len >= 11U) && (FORMAT_TYPE_I == d[5]) && (0U != (d[6] & 0x01U));
                            cur.channels = (len >= 11U) ? d[10] : 0U;
                        }
                        else
                        {
                            /* wFormatTag = PCM (0x0001) */
                            cur.pcm = (len >= 7U) && (0x01U == d[5]) && (0x00U == d[6]);
                        }
                    }
                    else if ((AS_FORMAT_TYPE == d[2]) && (FORMAT_TYPE_I == d[3]))
                    {
                        if (p_ac->uac2)
                        {
                            cur.subframe = d[4];      /* bSubslotSize   */
                            cur.bits     = d[5];      /* bBitResolution */
                        }
                        else if (len >= 8U)
                        {
                            cur.channels = d[4];      /* bNrChannels    */
                            cur.subframe = d[5];      /* bSubframeSize  */
                            cur.bits     = d[6];      /* bBitResolution */
                            cur.p_format = d;
                        }
                        else
                        {
                            /* Malformed. */
                        }
                    }
                    else
                    {
                        /* Not needed. */
                    }
                }
                else
                {
                    /* Some other class. */
                }

                break;
            }

            case DESC_ENDPOINT:
            {
                /* First isochronous OUT endpoint of the alternate setting. */
                if (in_as && (NULL == cur.p_ep) && (len >= 7U) && (0U == (d[2] & 0x80U)) && (0x01U == (d[3] & 0x03U)))
                {
                    cur.p_ep = d;
                }

                break;
            }

            default:
            {
                break;
            }
        }

        off += len;
    }

    if (in_as)
    {
        consider_alt(&cur, p_ac->uac2, p_best);
    }

    return p_best->score >= 0;
}

/* ------------------------------------------------------------------------- */
/* Control requests                                                           */
/* ------------------------------------------------------------------------- */

static void stop_streaming (void)
{
    g_streaming = false;
    g_dev_addr  = 0;
}

/* Issue a control transfer on the default pipe and wait for it to finish.
 * Other events received meanwhile are dropped (nothing is streaming yet),
 * except a detach, which aborts the setup. */
static fsp_err_t ctrl_xfer (uint16_t request_type, uint16_t value, uint16_t index, uint16_t length)
{
    usb_setup_t setup =
    {
        .request_type   = request_type,
        .request_value  = value,
        .request_index  = index,
        .request_length = length,
    };

    fsp_err_t err = R_USB_HostControlTransfer(&g_basic1_ctrl, &setup, g_ctrl_buf, g_dev_addr);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    for (;;)
    {
        usb_event_info_t ev;

        if (pdTRUE != xQueueReceive(g_aud_queue, &ev, pdMS_TO_TICKS(AUDIO_CTRL_TIMEOUT_MS)))
        {
            return FSP_ERR_TIMEOUT;
        }

        if (USB_STATUS_REQUEST_COMPLETE == ev.event)
        {
            return (USB_SETUP_STATUS_ACK == ev.status) ? FSP_SUCCESS : FSP_ERR_USB_FAILED;
        }

        if (USB_STATUS_DETACH == ev.event)
        {
            printf("AUDIO: device detached during setup\n");
            stop_streaming();

            return FSP_ERR_USB_FAILED;
        }
    }
}

static fsp_err_t set_interface (uint8_t iface, uint8_t alt)
{
    return ctrl_xfer(USB_SET_INTERFACE | USB_HOST_TO_DEV | USB_STANDARD | USB_INTERFACE, alt, iface, 0);
}

static fsp_err_t set_sample_rate (const ac_info_t * p_ac, const as_choice_t * p_cfg)
{
    uint32_t rate = p_cfg->rate;

    if (!p_ac->uac2)
    {
        /* Audio 1.0: SET_CUR SAMPLING_FREQ_CONTROL on the endpoint (3 bytes). */
        g_ctrl_buf[0] = (uint8_t) rate;
        g_ctrl_buf[1] = (uint8_t) (rate >> 8);
        g_ctrl_buf[2] = (uint8_t) (rate >> 16);

        return ctrl_xfer(REQ_SET_CUR | USB_HOST_TO_DEV | USB_CLASS | USB_ENDPOINT,
                         0x0100U, p_cfg->alt.p_ep[2], 3);
    }

    /* Audio 2.0: SET_CUR SAM_FREQ_CONTROL on the clock source feeding the
     * input terminal that the streaming interface is linked to (4 bytes). */
    uint8_t clock_id = p_ac->first_clock;

    for (uint8_t i = 0; i < p_ac->num_it; i++)
    {
        if (p_ac->it_id[i] == p_cfg->alt.terminal_link)
        {
            clock_id = p_ac->it_clock[i];
        }
    }

    if (0U == clock_id)
    {
        return FSP_ERR_NOT_FOUND;
    }

    g_ctrl_buf[0] = (uint8_t) rate;
    g_ctrl_buf[1] = (uint8_t) (rate >> 8);
    g_ctrl_buf[2] = (uint8_t) (rate >> 16);
    g_ctrl_buf[3] = (uint8_t) (rate >> 24);

    return ctrl_xfer(REQ_SET_CUR | USB_HOST_TO_DEV | USB_CLASS | USB_INTERFACE,
                     0x0100U, (uint16_t) ((clock_id << 8) | p_ac->ac_iface), 4);
}

/* Unmute every feature unit and set its volume to 0 dB (or the device's
 * maximum if that is lower). Game volume is then controlled from Doom's
 * menu. Units or channels without these controls simply STALL. */
static void unmute_feature_units (const ac_info_t * p_ac)
{
    for (uint8_t u = 0; u < p_ac->num_fu; u++)
    {
        uint16_t index = (uint16_t) ((p_ac->fu_id[u] << 8) | p_ac->ac_iface);

        for (uint8_t ch = 0; ch <= 2U; ch++)
        {
            int16_t volume = 0;

            g_ctrl_buf[0] = 0;         /* Unmuted */
            (void) ctrl_xfer(REQ_SET_CUR | USB_HOST_TO_DEV | USB_CLASS | USB_INTERFACE,
                             (uint16_t) ((FU_MUTE_CONTROL << 8) | ch), index, 1);

            if (0U == g_dev_addr)
            {
                return;                /* Detached */
            }

            if (!p_ac->uac2 &&
                (FSP_SUCCESS == ctrl_xfer(REQ_GET_MAX | USB_DEV_TO_HOST | USB_CLASS | USB_INTERFACE,
                                          (uint16_t) ((FU_VOLUME_CONTROL << 8) | ch), index, 2)))
            {
                int16_t max = (int16_t) (g_ctrl_buf[0] | (g_ctrl_buf[1] << 8));
                if (max < volume)
                {
                    volume = max;
                }
            }

            g_ctrl_buf[0] = (uint8_t) volume;
            g_ctrl_buf[1] = (uint8_t) ((uint16_t) volume >> 8);
            (void) ctrl_xfer(REQ_SET_CUR | USB_HOST_TO_DEV | USB_CLASS | USB_INTERFACE,
                             (uint16_t) ((FU_VOLUME_CONTROL << 8) | ch), index, 2);

            if (0U == g_dev_addr)
            {
                return;
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Streaming                                                                  */
/* ------------------------------------------------------------------------- */

/* Render one block from the Doom mixer and convert it to the device format. */
static void render_block (uint8_t * p_out)
{
    uint32_t frames   = g_frames_per_block;
    uint32_t channels = g_cfg.alt.channels;
    uint32_t subframe = g_cfg.alt.subframe;

    audio_mixer_render(g_mix_buf, frames);

    for (uint32_t i = 0; i < frames; i++)
    {
        int32_t l = g_mix_buf[(2U * i)];
        int32_t r = g_mix_buf[(2U * i) + 1U];

        if (1U == channels)
        {
            l = (l + r) / 2;
        }

        for (uint32_t c = 0; c < channels; c++)
        {
            int32_t s = (0U == c) ? l : r;

            /* Little-endian, MSB justified in the subframe */
            for (uint32_t b = 0; b < (subframe - 2U); b++)
            {
                *p_out++ = 0;
            }

            *p_out++ = (uint8_t) s;
            *p_out++ = (uint8_t) ((uint32_t) s >> 8);
        }
    }
}

static void send_block (uint8_t idx)
{
    fsp_err_t err = R_USB_Write(&g_basic1_ctrl, g_tx_buf[idx], g_block_bytes, g_dev_addr);

    if (FSP_SUCCESS != err)
    {
        if (0U == (g_write_errors++ & 0x3FU))
        {
            printf("AUDIO: R_USB_Write failed (%d), %lu errors\n", (int) err, (unsigned long) g_write_errors);
        }
    }
}

static void on_configured (const usb_event_info_t * p_ev)
{
    ac_info_t ac;

    g_dev_addr     = p_ev->device_address;
    g_write_errors = 0;

    uint8_t * p_cfg = g_p_usb_haud_config_table[p_ev->module_number];
    if (NULL == p_cfg)
    {
        printf("AUDIO: no configuration descriptor\n");

        return;
    }

    if (!parse_config(p_cfg, &ac, &g_cfg))
    {
        printf("AUDIO: no usable playback format (need PCM 16-32 bit, 1-2 ch, 16-48 kHz)\n");

        return;
    }

    printf("AUDIO: USB Audio %s device, addr %u: interface %u alt %u, %lu Hz, %u ch, %u-bit (%u-byte), EP 0x%02x, %lu bytes/frame\n",
           ac.uac2 ? "2.0" : "1.0", g_dev_addr, g_cfg.alt.iface, g_cfg.alt.alt, (unsigned long) g_cfg.rate,
           g_cfg.alt.channels, g_cfg.alt.bits, g_cfg.alt.subframe, g_cfg.alt.p_ep[2], (unsigned long) g_cfg.packet);

    fsp_err_t err = set_interface(g_cfg.alt.iface, g_cfg.alt.alt);
    if (FSP_SUCCESS != err)
    {
        printf("AUDIO: SET_INTERFACE failed (%d)\n", (int) err);

        return;
    }

    err = set_sample_rate(&ac, &g_cfg);
    if ((FSP_SUCCESS != err) && (0U != g_dev_addr))
    {
        /* Fixed-rate devices often STALL this; carry on. */
        printf("AUDIO: set sample rate not accepted (%d), continuing\n", (int) err);
    }

    if (0U == g_dev_addr)
    {
        return;
    }

    unmute_feature_units(&ac);

    if (0U == g_dev_addr)
    {
        return;
    }

    /* Make the pipe send exactly one frame's worth of audio per packet. */
    g_cfg.alt.p_ep[4] = (uint8_t) g_cfg.packet;
    g_cfg.alt.p_ep[5] = (uint8_t) (g_cfg.packet >> 8);

    err = R_USB_HAUD_PipeSet(&g_basic1_ctrl, g_cfg.alt.iface, g_cfg.alt.alt, g_dev_addr);
    if (FSP_SUCCESS != err)
    {
        printf("AUDIO: R_USB_HAUD_PipeSet failed (%d)\n", (int) err);

        return;
    }

    g_frames_per_block = (g_cfg.rate / 1000U) * AUDIO_BLOCK_MS;
    g_block_bytes      = g_cfg.packet * AUDIO_BLOCK_MS;

    audio_mixer_set_output_rate(g_cfg.rate);

    render_block(g_tx_buf[0]);
    render_block(g_tx_buf[1]);

    g_streaming = true;
    g_next_buf  = 1;
    send_block(0);

    printf("AUDIO: streaming\n");
}

static void on_write_complete (void)
{
    if (!g_streaming)
    {
        return;
    }

    /* Queue the pre-rendered buffer first so the isochronous stream has no
     * gap, then refill the buffer that has just been sent. */
    uint8_t done = (uint8_t) (g_next_buf ^ 1U);

    send_block(g_next_buf);
    render_block(g_tx_buf[done]);
    g_next_buf = done;
}

/* FSP 6.5 usb_haud_class_check() records the descriptors but never writes
 * the check result (*table[3]), which the host manager pre-sets to
 * USB_ERROR, so under FreeRTOS the audio driver never claims a device.
 * Wrap it and report success, as r_usb_hhid does. */
static void haud_class_check_fixed (usb_utr_t * ptr, uint16_t ** table)
{
    usb_haud_class_check(ptr, table);
    *table[3] = USB_OK;
}

static void install_class_check_fix (void)
{
    bool found = false;

    taskENTER_CRITICAL();
    for (uint16_t md = 0; md < g_usb_hstd_device_num[USB_IP0]; md++)
    {
        usb_hcdreg_t * p_drv = &g_usb_hstd_device_drv[USB_IP0][md];

        if ((USB_IFCLS_AUD == p_drv->ifclass) && ((usb_cb_check_t) &usb_haud_class_check == p_drv->classcheck))
        {
            p_drv->classcheck = (usb_cb_check_t) &haud_class_check_fixed;
            found             = true;
        }
    }

    taskEXIT_CRITICAL();

    if (!found)
    {
        printf("AUDIO: HAUD class driver not registered on USBFS - check USB Multiport setting\n");
    }
}

static void audio_task (void * pvParameters)
{
    FSP_PARAMETER_NOT_USED(pvParameters);

    /* Let the game pad task finish opening the USBHS instance first; the
     * FSP creates its shared USB RTOS tasks on the first R_USB_Open(). */
    while (!usb_gamepad_host_ready())
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    fsp_err_t err = R_USB_Open(&g_basic1_ctrl, &g_basic1_cfg);
    if (FSP_SUCCESS != err)
    {
        printf("AUDIO: R_USB_Open failed (%d)\n", (int) err);
        vTaskDelete(NULL);
    }

    install_class_check_fix();

    printf("AUDIO: host started on USBFS port, waiting for USB headphones\n");

    for (;;)
    {
        usb_event_info_t ev;

        if (pdTRUE != xQueueReceive(g_aud_queue, &ev, portMAX_DELAY))
        {
            continue;
        }

        switch (ev.event)
        {
            case USB_STATUS_CONFIGURED:
            {
                if (USB_CLASS_HAUD == ev.type)
                {
                    on_configured(&ev);
                }
                else
                {
                    printf("AUDIO: device on USBFS configured as class %d, not audio\n", (int) ev.type);
                }

                break;
            }

            case USB_STATUS_WRITE_COMPLETE:
            {
                if (FSP_SUCCESS != ev.status)
                {
                    g_write_errors++;
                }

                on_write_complete();
                break;
            }

            case USB_STATUS_DETACH:
            {
                printf("AUDIO: device detached\n");
                stop_streaming();
                break;
            }

            case USB_STATUS_NOT_SUPPORT:
            {
                printf("AUDIO: unsupported device attached to USBFS port\n");
                break;
            }

            case USB_STATUS_OVERCURRENT:
            {
                printf("AUDIO: over current on USBFS VBUS!\n");
                break;
            }

            default:
            {
                break;
            }
        }
    }
}

void usb_audio_task_start (void)
{
    g_aud_queue = xQueueCreateStatic(AUDIO_EVENT_QUEUE_LEN, sizeof(usb_event_info_t),
                                     g_aud_queue_storage, &g_aud_queue_mem);

    (void) xTaskCreateStatic(audio_task, "USB audio", AUDIO_TASK_STACK_WORDS, NULL, AUDIO_TASK_PRIORITY,
                             g_aud_task_stack, &g_aud_task_tcb);
}
