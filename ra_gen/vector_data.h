/* generated vector header file - do not edit */
        #ifndef VECTOR_DATA_H
        #define VECTOR_DATA_H
        #ifdef __cplusplus
        extern "C" {
        #endif
                /* Number of interrupts allocated */
        #ifndef VECTOR_DATA_IRQ_COUNT
        #define VECTOR_DATA_IRQ_COUNT    (4)
        #endif
        /* ISR prototypes */
        void glcdc_line_detect_isr(void);
        void usbfs_interrupt_handler(void);
        void usbfs_resume_handler(void);
        void usbhs_interrupt_handler(void);

        /* Vector table allocations */
        #define VECTOR_NUMBER_GLCDC_LINE_DETECT ((IRQn_Type) 0) /* GLCDC LINE DETECT (Specified line) */
        #define GLCDC_LINE_DETECT_IRQn          ((IRQn_Type) 0) /* GLCDC LINE DETECT (Specified line) */
        #define VECTOR_NUMBER_USBFS_INT ((IRQn_Type) 1) /* USBFS INT (USBFS interrupt) */
        #define USBFS_INT_IRQn          ((IRQn_Type) 1) /* USBFS INT (USBFS interrupt) */
        #define VECTOR_NUMBER_USBFS_RESUME ((IRQn_Type) 2) /* USBFS RESUME (USBFS resume interrupt) */
        #define USBFS_RESUME_IRQn          ((IRQn_Type) 2) /* USBFS RESUME (USBFS resume interrupt) */
        #define VECTOR_NUMBER_USBHS_USB_INT_RESUME ((IRQn_Type) 3) /* USBHS USB INT RESUME (USBHS interrupt) */
        #define USBHS_USB_INT_RESUME_IRQn          ((IRQn_Type) 3) /* USBHS USB INT RESUME (USBHS interrupt) */
        /* The number of entries required for the ICU vector table. */
        #define BSP_ICU_VECTOR_NUM_ENTRIES (4)

        #ifdef __cplusplus
        }
        #endif
        #endif /* VECTOR_DATA_H */
