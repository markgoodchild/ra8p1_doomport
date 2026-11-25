/* generated thread header file - do not edit */
#ifndef NEW_THREAD0_H_
#define NEW_THREAD0_H_
#include "bsp_api.h"
                #include "FreeRTOS.h"
                #include "task.h"
                #include "semphr.h"
                #include "hal_data.h"
                #ifdef __cplusplus
                extern "C" void new_thread0_entry(void * pvParameters);
                #else
                extern void new_thread0_entry(void * pvParameters);
                #endif
#include "r_usb_basic.h"
#include "r_usb_basic_api.h"
#include "r_usb_haud_api.h"
#include "r_usb_basic.h"
#include "r_usb_basic_api.h"
#include "r_usb_hhid_api.h"
#include "r_usb_hhid.h"
FSP_HEADER
/* Basic on USB Instance. */
extern const usb_instance_t g_basic1;

/** Access the USB instance using these structures when calling API functions directly (::p_api is not used). */
extern usb_instance_ctrl_t g_basic1_ctrl;
extern const usb_cfg_t g_basic1_cfg;

#ifndef NULL
void NULL(void *);
#endif

#if 0 == BSP_CFG_RTOS
#ifndef usb_audio_callback
void usb_audio_callback(usb_callback_args_t *);
#endif
#endif

#if 2 == BSP_CFG_RTOS
#ifndef usb_audio_callback
void usb_audio_callback(usb_event_info_t *, usb_hdl_t, usb_onoff_t);
#endif
#endif
/** Audio Driver on USB Instance. */
/* Basic on USB Instance. */
extern const usb_instance_t g_basic0;

/** Access the USB instance using these structures when calling API functions directly (::p_api is not used). */
extern usb_instance_ctrl_t g_basic0_ctrl;
extern const usb_cfg_t g_basic0_cfg;

#ifndef NULL
void NULL(void *);
#endif

#if 0 == BSP_CFG_RTOS
#ifndef usb_host_callback
void usb_host_callback(usb_callback_args_t *);
#endif
#endif

#if 2 == BSP_CFG_RTOS
#ifndef usb_host_callback
void usb_host_callback(usb_event_info_t *, usb_hdl_t, usb_onoff_t);
#endif
#endif
/** HID Driver on USB Instance. */
FSP_FOOTER
#endif /* NEW_THREAD0_H_ */
