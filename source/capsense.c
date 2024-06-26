/*******************************************************************************
* File Name: capsense.c
*
* Description: This file contains the task that handles touch sensing.
*
* Related Document: README.md
*
********************************************************************************
* Copyright 2021-2024, Cypress Semiconductor Corporation (an Infineon company) or
* an affiliate of Cypress Semiconductor Corporation.  All rights reserved.
*
* This software, including source code, documentation and related
* materials ("Software") is owned by Cypress Semiconductor Corporation
* or one of its affiliates ("Cypress") and is protected by and subject to
* worldwide patent protection (United States and foreign),
* United States copyright laws and international treaty provisions.
* Therefore, you may use this Software only as provided in the license
* agreement accompanying the software package from which you
* obtained this Software ("EULA").
* If no EULA applies, Cypress hereby grants you a personal, non-exclusive,
* non-transferable license to copy, modify, and compile the Software
* source code solely for use in connection with Cypress's
* integrated circuit products.  Any reproduction, modification, translation,
* compilation, or representation of this Software except as specified
* above is prohibited without the express written permission of Cypress.
*
* Disclaimer: THIS SOFTWARE IS PROVIDED AS-IS, WITH NO WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, NONINFRINGEMENT, IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. Cypress
* reserves the right to make changes to the Software without notice. Cypress
* does not assume any liability arising out of the application or use of the
* Software or any product or circuit described in the Software. Cypress does
* not authorize its products for use in any products where a malfunction or
* failure of the Cypress product may reasonably be expected to result in
* significant property damage, injury or death ("High Risk Product"). By
* including Cypress's product in a High Risk Product, the manufacturer
* of such system or application assumes all risk of such use and in doing
* so agrees to indemnify Cypress against all liability.
*******************************************************************************/

/*******************************************************************************
* Header files includes
*******************************************************************************/
#include "cybsp.h"
#include "cyhal.h"
#include "cycfg.h"
#include "cy_retarget_io.h"
#include "cycfg_capsense.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "timers.h"
#include "board.h"
#include "capsense.h"
#include "bt_app.h"


/*******************************************************************************
* Macros
*******************************************************************************/
#define CAPSENSE_INTERRUPT_PRIORITY     (7u)

#define EZI2C_INTERRUPT_PRIORITY        (6u)    /* EZI2C interrupt priority must be
                                             * higher than CapSense interrupt
                                             */
#define CAPSENSE_SCAN_INTERVAL_MS       (10u)   /* in milliseconds*/

#define CAPSENSE_BUTTON_COUNT           (2u)


/*******************************************************************************
* Global Variables
*******************************************************************************/
QueueHandle_t capsense_command_q;
TimerHandle_t scan_timer_handle;

/* Variables used for storing slider data */
capsense_data_t capsense_data = {0};

cy_stc_scb_ezi2c_context_t ezi2c_context;
cyhal_ezi2c_t sEzI2C;
cyhal_ezi2c_slave_cfg_t sEzI2C_sub_cfg;
cyhal_ezi2c_cfg_t sEzI2C_cfg;

/* SysPm callback params */
cy_stc_syspm_callback_params_t callback_params =
{
    .base       = CYBSP_CSD_HW,
    .context    = &cy_capsense_context
};

cy_stc_syspm_callback_t capsense_deep_sleep_cb =
{
    Cy_CapSense_DeepSleepCallback,
    CY_SYSPM_DEEPSLEEP,
    (CY_SYSPM_SKIP_CHECK_FAIL | CY_SYSPM_SKIP_BEFORE_TRANSITION | CY_SYSPM_SKIP_AFTER_TRANSITION),
    &callback_params,
    NULL,
    NULL
};


/*******************************************************************************
* Function Prototypes
*******************************************************************************/
static cy_status capsense_init(void);
static void capsense_tuner_init(void);
static void capsense_process(void);
static void capsense_isr(void);
static void capsense_end_of_scan_callback(cy_stc_active_scan_sns_t* active_scan_sns_ptr);
static void capsense_timer_callback(TimerHandle_t xTimer);


/*******************************************************************************
* Function Definitions
*******************************************************************************/

/*******************************************************************************
* Function Name: capsense_task
********************************************************************************
* Summary:
*  Task that initializes the CapSense block and processes the touch input.
*
* Parameters:
*  void *param : Task parameter defined during task creation (unused)
*
* Return:
*   None
*
*******************************************************************************/
void capsense_task(void* param)
{
    BaseType_t rtos_api_result = pdFAIL;
    cy_status status = CY_RSLT_SUCCESS;
    capsense_command_t capsense_cmd;

    /* Remove warning for unused parameter */
    (void)param;

    /* Initialize timer for periodic CapSense scan */
    scan_timer_handle = xTimerCreate ("Scan Timer", CAPSENSE_SCAN_INTERVAL_MS,
                                      pdTRUE, NULL, capsense_timer_callback);

    if(NULL == scan_timer_handle)
    {
        printf("CapSense scan timer initialization failed!\r\n");
        CY_ASSERT(0u);
    }

    /* Setup communication between Tuner GUI and PSoC 6 MCU */
    capsense_tuner_init();

    /* Initialize CapSense block */
    status = capsense_init();

    if(CY_RSLT_SUCCESS != status)
    {
        printf("CapSense initialization failed!\r\n");
        CY_ASSERT(0u);
    }

    /* Start the timer */
    rtos_api_result = xTimerStart(scan_timer_handle, 0u);
    if(pdFAIL == rtos_api_result)
    {
        printf("Failed to start CapSense scan timer!\r\n");
        CY_ASSERT(0u);
    }

    /* Repeatedly running part of the task */
    for(;;)
    {
        /* Block until a CapSense command has been received over queue */
        rtos_api_result = xQueueReceive(capsense_command_q, &capsense_cmd,
                                                            portMAX_DELAY);

        /* Command has been received from capsense_cmd */
        if(pdTRUE == rtos_api_result)
        {
            /* Check if CapSense is busy with a previous scan */
            if(CY_CAPSENSE_NOT_BUSY == Cy_CapSense_IsBusy(&cy_capsense_context))
            {
                switch(capsense_cmd)
                {
                    case CAPSENSE_SCAN:
                    {
                        /* Start scan */
                        if(CY_RSLT_SUCCESS != Cy_CapSense_ScanAllWidgets(&cy_capsense_context))
                        {
                            printf("CapSense scanning failed!\r\n");
                        }
                        break;
                    }
                    case CAPSENSE_PROCESS:
                    {
                        /* Process all widgets */
                        if(CY_RSLT_SUCCESS == Cy_CapSense_ProcessAllWidgets(&cy_capsense_context))
                        {
                            capsense_process();
                        }
                        else
                        {
                            printf("CapSense processing failed!\r\n");
                        }

                        /* Establishes synchronized operation between the
                         * Capsese middleware and the CapSense Tuner tool.
                         */
                        Cy_CapSense_RunTuner(&cy_capsense_context);
                        break;
                    }
                    /* Invalid command */
                    default:
                    {
                        break;
                    }
                }
            }
        }

    }
}


/*******************************************************************************
* Function Name: capsense_process
********************************************************************************
* Summary:
*  This function processes the CapSense touch input and sends set the commands.
* 
* Parameters:
*  None
*
* Return:
*  None
*
*******************************************************************************/
static void capsense_process(void)
{
    /* Variables used to store touch information */
    uint32_t button0_status = 0;
    uint32_t button1_status = 0;
    uint16_t slider_pos = 0;
    uint8_t slider_value = 0;
    uint8_t slider_touched = 0;
    cy_stc_capsense_touch_t *slider_touch;

    /* Variables used to store previous touch information */
    static uint32_t button0_status_prev = 0;
    static uint32_t button1_status_prev = 0;
    static uint16_t slider_pos_perv = 0;

    /* Variables used for storing command and data for LED Task */
    led_command_data_t led_cmd_data;
    bool send_led_command = false;


    bool send_bt_command = false;
     /* Total capsense button in the kit */
    capsense_data.buttoncount = CAPSENSE_BUTTON_COUNT;

    /* Get Button0 status */
    button0_status = Cy_CapSense_IsSensorActive(
                        CY_CAPSENSE_BUTTON0_WDGT_ID,
                        CY_CAPSENSE_BUTTON0_SNS0_ID,
                        &cy_capsense_context);

    /* Get Button1 status */
    button1_status = Cy_CapSense_IsSensorActive(
                        CY_CAPSENSE_BUTTON1_WDGT_ID,
                        CY_CAPSENSE_BUTTON1_SNS0_ID,
                        &cy_capsense_context);

    /* Get slider status */
    slider_touch = Cy_CapSense_GetTouchInfo(
                        CY_CAPSENSE_LINEARSLIDER0_WDGT_ID,
                        &cy_capsense_context);
    slider_pos = slider_touch->ptrPosition->x;
    slider_touched = slider_touch->numPosition;

    /* Detect new touch on Button0 */
    if((0u != button0_status) && (0u == button0_status_prev))
    {
        led_cmd_data.command = LED_TURN_ON;
        send_led_command = true;
        capsense_data.buttonstatus1 = 1u;
        send_bt_command = true;
    }

    /* Detect new touch on Button1 */
    if((0u != button1_status) && (0u == button1_status_prev))
    {
        led_cmd_data.command = LED_TURN_OFF;
        send_led_command = true;
        capsense_data.buttonstatus1 = 2u;
        send_bt_command = true;
    }
    /* Detect new touch on slider */
    if((0u != slider_touched) && (slider_pos_perv != slider_pos ))
    {
        /* Slider value in percentage */
        slider_value = (slider_pos * 100) /
        cy_capsense_context.ptrWdConfig[CY_CAPSENSE_LINEARSLIDER0_WDGT_ID].xResolution;
        led_cmd_data.command = LED_SET_BRIGHTNESS;
        /* Setting brightness value */
        led_cmd_data.brightness = slider_value;
        send_led_command = true;
        /* Setting ble app data value */
        capsense_data.sliderdata = slider_value;
        send_bt_command = true;

    }

    /* Send command to update LED state if required */
    if(send_led_command)
    {
        xQueueSendToBack(led_command_data_q, &led_cmd_data, 0u);
    }

    if(send_bt_command)
    {
        xTaskNotify(bt_task_handle, 1, eSetValueWithoutOverwrite);
    }

    /* Update previous touch status */
    button0_status_prev = button0_status;
    button1_status_prev = button1_status;
    slider_pos_perv = slider_pos;
}


/*******************************************************************************
* Function Name: capsense_init
********************************************************************************
* Summary:
*  This function initializes the CSD HW block, and configures the CapSense
*  interrupt.
*
* Parameters:
*  None
*
* Return:
*   cy_rslt_t  Result status
*******************************************************************************/
static cy_status capsense_init(void)
{
    cy_status status = CY_RSLT_SUCCESS;

    /* CapSense interrupt configuration parameters */
    static const cy_stc_sysint_t capSense_intr_config =
    {
        .intrSrc = csd_interrupt_IRQn,
        .intrPriority = CAPSENSE_INTERRUPT_PRIORITY,
    };

    /*Initialize CapSense Data structures */
    status = Cy_CapSense_Init(&cy_capsense_context);
    if (CY_RSLT_SUCCESS != status)
    {
        return status;
    }

    /* Initialize CapSense interrupt */
    cyhal_system_set_isr(csd_interrupt_IRQn,csd_interrupt_IRQn,
                                    CAPSENSE_INTERRUPT_PRIORITY, &capsense_isr);
    NVIC_ClearPendingIRQ(capSense_intr_config.intrSrc);
    NVIC_EnableIRQ(capSense_intr_config.intrSrc);

    /* Initialize the CapSense deep sleep callback functions. */
    Cy_CapSense_Enable(&cy_capsense_context);
    Cy_SysPm_RegisterCallback(&capsense_deep_sleep_cb);
    /* Register end of scan callback */
    status = Cy_CapSense_RegisterCallback(CY_CAPSENSE_END_OF_SCAN_E,
                                              capsense_end_of_scan_callback, &cy_capsense_context);
    if (CY_RSLT_SUCCESS != status)
    {
        return status;
    }
    /* Initialize the CapSense firmware modules. */
    status = Cy_CapSense_Enable(&cy_capsense_context);
    if (CY_RSLT_SUCCESS != status)
    {
        return status;
    }

    return status;
}


/*******************************************************************************
* Function Name: capsense_end_of_scan_callback
********************************************************************************
* Summary:
*  CapSense end of scan callback function. This function sends a command to
*  CapSense task to process scan.
*
* Parameters:
*  cy_stc_active_scan_sns_t * active_scan_sns_ptr (unused)
*
* Return:
*   None
*
*******************************************************************************/
static void capsense_end_of_scan_callback(cy_stc_active_scan_sns_t* active_scan_sns_ptr)
{
    BaseType_t xYieldRequired;

    (void)active_scan_sns_ptr;

    /* Send command to process CapSense data */
    capsense_command_t commmand = CAPSENSE_PROCESS;
    xYieldRequired = xQueueSendToBackFromISR(capsense_command_q, &commmand, 0u);
    portYIELD_FROM_ISR(xYieldRequired);
}


/*******************************************************************************
* Function Name: capsense_timer_callback
********************************************************************************
* Summary:
*  CapSense timer callback. This function sends a command to start CapSense
*  scan.
*
* Parameters:
*  TimerHandle_t xTimer (unused)
*
* Return:
*   None
*
*******************************************************************************/
static void capsense_timer_callback(TimerHandle_t xTimer)
{
    Cy_CapSense_Wakeup(&cy_capsense_context);
    capsense_command_t command = CAPSENSE_SCAN;
    BaseType_t xYieldRequired;

    (void)xTimer;

    /* Send command to start CapSense scan */
    xYieldRequired = xQueueSendToBackFromISR(capsense_command_q, &command, 0u);
    portYIELD_FROM_ISR(xYieldRequired);
}


/*******************************************************************************
* Function Name: capsense_isr
********************************************************************************
* Summary:
*  Wrapper function for handling interrupts from CSD block.
*
* Parameters:
*  None
*
* Return:
*   None
*
*******************************************************************************/
static void capsense_isr(void)
{
    Cy_CapSense_InterruptHandler(CYBSP_CSD_HW, &cy_capsense_context);
}


/*******************************************************************************
* Function Name: capsense_tuner_init
********************************************************************************
* Summary:
*  Initializes communication between Tuner GUI and PSoC 6 MCU.
*
* Parameters:
*  None
*
* Return:
*   None
*
*******************************************************************************/
static void capsense_tuner_init(void)
{
    cy_rslt_t result = CY_RSLT_SUCCESS;

    /* Configure CapSense Tuner as EzI2C Slave */
    sEzI2C_sub_cfg.buf = (uint8 *)&cy_capsense_tuner;
    sEzI2C_sub_cfg.buf_rw_boundary = sizeof(cy_capsense_tuner);
    sEzI2C_sub_cfg.buf_size = sizeof(cy_capsense_tuner);
    sEzI2C_sub_cfg.slave_address = 8U;

    sEzI2C_cfg.data_rate = CYHAL_EZI2C_DATA_RATE_400KHZ;
    sEzI2C_cfg.enable_wake_from_sleep = true;
    sEzI2C_cfg.slave1_cfg = sEzI2C_sub_cfg;
    sEzI2C_cfg.sub_address_size = CYHAL_EZI2C_SUB_ADDR16_BITS;
    sEzI2C_cfg.two_addresses = false;
    result = cyhal_ezi2c_init( &sEzI2C, CYBSP_I2C_SDA, CYBSP_I2C_SCL, NULL, &sEzI2C_cfg);
    if (result != CY_RSLT_SUCCESS)
    {
        printf("CapSense tuner initialization failed!\r\n");
        CY_ASSERT(0u);
    }

}


/* END OF FILE [] */