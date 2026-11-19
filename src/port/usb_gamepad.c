/*
 * USB host game pad driver for Sony DualSense (PS5) and DualShock 4 (PS4)
 * controllers, connected to the EK-RA8P1 USB high-speed port (USBHS, IP1).
 *
 * The FSP USB Host HID class driver (r_usb_hhid) enumerates the controller -
 * both are composite devices (audio + HID) and FSP picks the HID interface.
 * This task then keeps an interrupt IN transfer pending, decodes every input
 * report and publishes a pad_state_t snapshot for the Doom thread.
 *
 * Note: the FSP HHID driver only supports full speed, so the USBHS
 * controller runs in full-speed mode. That is ample for a game pad.
 */
#include <stdio.h>
#include <string.h>

#include "new_thread0.h"
#include "queue.h"

#include "ra8_port.h"

#define USB_TASK_STACK_WORDS   (1024)
#define USB_TASK_PRIORITY      (2)     /* Below the FSP HCD (4) / MGR (3) tasks, above Doom (1) */
#define USB_EVENT_QUEUE_LEN    (16)

#define SONY_VID               (0x054CU)
#define PID_DS4_V1             (0x05C4U)
#define PID_DS4_V2             (0x09CCU)
#define PID_DS4_DONGLE         (0x0BA0U)
#define PID_DUALSENSE          (0x0CE6U)
#define PID_DUALSENSE_EDGE     (0x0DF2U)

typedef enum
{
    PAD_TYPE_UNKNOWN,
    PAD_TYPE_DUALSENSE,
    PAD_TYPE_DS4,
} pad_type_t;

/* Device descriptor captured by the FSP host stack during enumeration
 * (r_usb_hmanager.c). USB_DEVICESIZE is 20 bytes = 10 half words. */
extern uint16_t g_usb_hstd_device_descriptor[][10];

static QueueHandle_t  g_usb_queue;
static StaticQueue_t  g_usb_queue_mem;
static uint8_t        g_usb_queue_storage[USB_EVENT_QUEUE_LEN * sizeof(usb_event_info_t)];

static StaticTask_t   g_usb_task_tcb;
static StackType_t    g_usb_task_stack[USB_TASK_STACK_WORDS];

static uint8_t        g_rx_buf[64] BSP_ALIGN_VARIABLE(4);
static uint8_t        g_tx_buf[64] BSP_ALIGN_VARIABLE(4);

static uint8_t        g_dev_addr;
static uint16_t       g_rx_size;
static pad_type_t     g_pad_type;

static pad_state_t    g_state;         /* Protected by a critical section */

/* FSP USB event callback (RTOS mode). The event structure lives in a ring
 * owned by the driver, so it is copied into our own queue. */
void usb_host_callback (usb_event_info_t * p_event_info, usb_hdl_t cur_task, usb_onoff_t usb_state)
{
    FSP_PARAMETER_NOT_USED(cur_task);
    FSP_PARAMETER_NOT_USED(usb_state);

    if (NULL == g_usb_queue)
    {
        return;
    }

    if (xPortIsInsideInterrupt())
    {
        BaseType_t woken = pdFALSE;
        (void) xQueueSendFromISR(g_usb_queue, p_event_info, &woken);
        portYIELD_FROM_ISR(woken);
    }
    else
    {
        (void) xQueueSend(g_usb_queue, p_event_info, 0);
    }
}

void usb_gamepad_get_state (pad_state_t * p_state)
{
    taskENTER_CRITICAL();
    *p_state = g_state;
    taskEXIT_CRITICAL();
}

static uint32_t decode_hat (uint8_t hat)
{
    static const uint32_t s_hat[8] =
    {
        PAD_BTN_UP,
        PAD_BTN_UP | PAD_BTN_RIGHT,
        PAD_BTN_RIGHT,
        PAD_BTN_DOWN | PAD_BTN_RIGHT,
        PAD_BTN_DOWN,
        PAD_BTN_DOWN | PAD_BTN_LEFT,
        PAD_BTN_LEFT,
        PAD_BTN_UP | PAD_BTN_LEFT,
    };

    return (hat < 8U) ? s_hat[hat] : 0U;
}

/* Face/shoulder button bytes share the same layout on DualSense and DS4. */
static uint32_t decode_buttons (uint8_t b0, uint8_t b1)
{
    uint32_t buttons = decode_hat(b0 & 0x0FU);

    if (b0 & 0x10U) { buttons |= PAD_BTN_SQUARE;   }
    if (b0 & 0x20U) { buttons |= PAD_BTN_CROSS;    }
    if (b0 & 0x40U) { buttons |= PAD_BTN_CIRCLE;   }
    if (b0 & 0x80U) { buttons |= PAD_BTN_TRIANGLE; }
    if (b1 & 0x01U) { buttons |= PAD_BTN_L1;       }
    if (b1 & 0x02U) { buttons |= PAD_BTN_R1;       }
    if (b1 & 0x04U) { buttons |= PAD_BTN_L2;       }
    if (b1 & 0x08U) { buttons |= PAD_BTN_R2;       }
    if (b1 & 0x10U) { buttons |= PAD_BTN_CREATE;   }
    if (b1 & 0x20U) { buttons |= PAD_BTN_OPTIONS;  }
    if (b1 & 0x40U) { buttons |= PAD_BTN_L3;       }
    if (b1 & 0x80U) { buttons |= PAD_BTN_R3;       }

    return buttons;
}

static void parse_report (const uint8_t * p, uint32_t len)
{
    pad_state_t s;

    /* Both controllers use input report 0x01 over USB. */
    if ((len < 11U) || (0x01U != p[0]))
    {
        return;
    }

    taskENTER_CRITICAL();
    s = g_state;
    taskEXIT_CRITICAL();

    s.lx = p[1];
    s.ly = p[2];
    s.rx = p[3];
    s.ry = p[4];

    if (PAD_TYPE_DS4 == g_pad_type)
    {
        /* [5] hat+face, [6] shoulders, [7] PS/touch, [8] L2, [9] R2 */
        s.buttons = decode_buttons(p[5], p[6]);
        if (p[7] & 0x01U) { s.buttons |= PAD_BTN_PS;    }
        if (p[7] & 0x02U) { s.buttons |= PAD_BTN_TOUCH; }
        s.l2 = p[8];
        s.r2 = p[9];
    }
    else
    {
        /* DualSense: [5] L2, [6] R2, [7] seq, [8] hat+face, [9] shoulders,
         * [10] PS/touch/mute */
        s.l2      = p[5];
        s.r2      = p[6];
        s.buttons = decode_buttons(p[8], p[9]);
        if (p[10] & 0x01U) { s.buttons |= PAD_BTN_PS;    }
        if (p[10] & 0x02U) { s.buttons |= PAD_BTN_TOUCH; }
    }

    s.connected = true;
    s.report_count++;

    taskENTER_CRITICAL();
    g_state = s;
    taskEXIT_CRITICAL();
}

/* Set the light bar to Doom red so it is obvious the controller was found. */
static void send_lightbar (void)
{
    uint32_t len;

    memset(g_tx_buf, 0, sizeof(g_tx_buf));

    if (PAD_TYPE_DS4 == g_pad_type)
    {
        g_tx_buf[0] = 0x05;            /* DS4 USB output report          */
        g_tx_buf[1] = 0x02;            /* Valid flags: light bar          */
        g_tx_buf[6] = 0xC0;            /* Red                             */
        g_tx_buf[7] = 0x00;            /* Green                           */
        g_tx_buf[8] = 0x00;            /* Blue                            */
        len         = 32;
    }
    else
    {
        g_tx_buf[0]  = 0x02;           /* DualSense USB output report     */
        g_tx_buf[2]  = 0x04 | 0x10;    /* Valid flags: light bar, player LEDs */
        g_tx_buf[44] = 0x04;           /* Player LED: centre              */
        g_tx_buf[45] = 0xC0;           /* Red                             */
        g_tx_buf[46] = 0x00;           /* Green                           */
        g_tx_buf[47] = 0x00;           /* Blue                            */
        len          = 48;
    }

    fsp_err_t err = R_USB_Write(&g_basic0_ctrl, g_tx_buf, len, g_dev_addr);
    if (FSP_SUCCESS != err)
    {
        printf("USB: light bar write failed (%d)\n", (int) err);
    }
}

static void start_read (void)
{
    fsp_err_t err = R_USB_Read(&g_basic0_ctrl, g_rx_buf, g_rx_size, g_dev_addr);
    if (FSP_SUCCESS != err)
    {
        printf("USB: R_USB_Read failed (%d)\n", (int) err);
    }
}

static void on_configured (const usb_event_info_t * p_ev)
{
    uint8_t  hid_type = 0;
    uint16_t mps      = 0;

    g_dev_addr = p_ev->device_address;

    uint16_t vid = g_usb_hstd_device_descriptor[p_ev->module_number][4];
    uint16_t pid = g_usb_hstd_device_descriptor[p_ev->module_number][5];

    (void) R_USB_HHID_TypeGet(&g_basic0_ctrl, &hid_type, g_dev_addr);
    (void) R_USB_HHID_MaxPacketSizeGet(&g_basic0_ctrl, &mps, USB_HID_IN, g_dev_addr);

    if ((0U == mps) || (mps > sizeof(g_rx_buf)))
    {
        mps = sizeof(g_rx_buf);
    }

    g_rx_size = mps;

    if ((SONY_VID == vid) && ((PID_DS4_V1 == pid) || (PID_DS4_V2 == pid) || (PID_DS4_DONGLE == pid)))
    {
        g_pad_type = PAD_TYPE_DS4;
    }
    else if ((SONY_VID == vid) && ((PID_DUALSENSE == pid) || (PID_DUALSENSE_EDGE == pid)))
    {
        g_pad_type = PAD_TYPE_DUALSENSE;
    }
    else
    {
        g_pad_type = PAD_TYPE_UNKNOWN;  /* Assume DualSense report layout */
    }

    printf("USB: HID device configured: addr %u, VID %04x PID %04x (%s), protocol %u, IN packet %u bytes\n",
           g_dev_addr, vid, pid,
           (PAD_TYPE_DS4 == g_pad_type) ? "DualShock 4" :
           (PAD_TYPE_DUALSENSE == g_pad_type) ? "DualSense" : "unknown, using DualSense layout",
           hid_type, mps);

    taskENTER_CRITICAL();
    memset(&g_state, 0, sizeof(g_state));
    g_state.lx = g_state.ly = g_state.rx = g_state.ry = 128;
    taskEXIT_CRITICAL();

    start_read();
    send_lightbar();
}

static void usb_task (void * pvParameters)
{
    FSP_PARAMETER_NOT_USED(pvParameters);

    fsp_err_t err = R_USB_Open(&g_basic0_ctrl, &g_basic0_cfg);
    if (FSP_SUCCESS != err)
    {
        printf("USB: R_USB_Open failed (%d)\n", (int) err);
        vTaskDelete(NULL);
    }

    printf("USB: host started on USBHS port, waiting for controller\n");

    for (;;)
    {
        usb_event_info_t ev;

        if (pdTRUE != xQueueReceive(g_usb_queue, &ev, portMAX_DELAY))
        {
            continue;
        }

        switch (ev.event)
        {
            case USB_STATUS_CONFIGURED:
                on_configured(&ev);
                break;

            case USB_STATUS_READ_COMPLETE:
                if (ev.device_address == g_dev_addr)
                {
                    parse_report(g_rx_buf, ev.data_size);
                    start_read();
                }
                break;

            case USB_STATUS_WRITE_COMPLETE:
                break;

            case USB_STATUS_DETACH:
                printf("USB: device detached\n");
                g_dev_addr = 0;
                taskENTER_CRITICAL();
                g_state.connected = false;
                g_state.buttons   = 0;
                taskEXIT_CRITICAL();
                break;

            case USB_STATUS_NOT_SUPPORT:
                printf("USB: unsupported device attached\n");
                break;

            case USB_STATUS_OVERCURRENT:
                printf("USB: over current on VBUS!\n");
                break;

            default:
                break;
        }
    }
}

void usb_gamepad_task_start (void)
{
    memset(&g_state, 0, sizeof(g_state));
    g_state.lx = g_state.ly = g_state.rx = g_state.ry = 128;

    g_usb_queue = xQueueCreateStatic(USB_EVENT_QUEUE_LEN, sizeof(usb_event_info_t),
                                     g_usb_queue_storage, &g_usb_queue_mem);

    (void) xTaskCreateStatic(usb_task, "USB pad", USB_TASK_STACK_WORDS, NULL, USB_TASK_PRIORITY,
                             g_usb_task_stack, &g_usb_task_tcb);
}
