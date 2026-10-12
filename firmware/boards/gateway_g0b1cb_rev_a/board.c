#include "board.h"
#include "stm32g0xx_hal.h"
#include "tusb.h"
#include "opensimgear/gateway_protocol.h"
#include <string.h>

#define TX_QUEUE_CAPACITY 8U
#define TX_TIMEOUT_MS 20U
#define TX_RETRY_MS 20U
#define CAN_RECOVERY_MS 1000U
static FDCAN_HandleTypeDef can;
static board_can_frame_t tx_queue[TX_QUEUE_CAPACITY];
static uint8_t tx_head, tx_count, tx_attempts;
static uint32_t tx_buffer, tx_started, tx_retry_at, recovery_at;
static bool tx_inflight, recovering;
static board_can_diagnostics_t diagnostics;
static const uint8_t dlc_sizes[16] = {0,1,2,3,4,5,6,7,8,12,16,20,24,32,48,64};

void board_fault(void) {
    HAL_GPIO_WritePin(GPIOB,GPIO_PIN_5,GPIO_PIN_SET); board_led(true);
    __disable_irq(); for (;;) { }
}
static void check(HAL_StatusTypeDef status) { if(status != HAL_OK) board_fault(); }
uint32_t board_millis(void) { return HAL_GetTick(); }
void board_led(bool on) { HAL_GPIO_WritePin(GPIOA,GPIO_PIN_5,on ? GPIO_PIN_SET : GPIO_PIN_RESET); }
bool board_usb_present(void) { return HAL_GPIO_ReadPin(GPIOB,GPIO_PIN_0) == GPIO_PIN_SET; }
void board_uid_hex(char out[25]) {
    static const char hex[] = "0123456789ABCDEF";
    const volatile uint8_t *uid = (const volatile uint8_t *)UID_BASE;
    for(unsigned i=0;i<12;i++) { out[2*i]=hex[uid[i]>>4]; out[2*i+1]=hex[uid[i]&15]; }
    out[24]='\0';
}

static bool can_start(void) {
    can.Instance=FDCAN1;
    can.Init.ClockDivider=FDCAN_CLOCK_DIV1;
    can.Init.FrameFormat=FDCAN_FRAME_FD_BRS;
    can.Init.Mode=FDCAN_MODE_NORMAL;
    /* Application retries are bounded; an absent bus peer cannot occupy a
     * controller mailbox indefinitely. */
    can.Init.AutoRetransmission=DISABLE;
    can.Init.TransmitPause=DISABLE;
    can.Init.ProtocolException=DISABLE;
    /* 64 MHz PCLK: 500 kbit/s arbitration, 2 Mbit/s data; 87.5% sample point. */
    can.Init.NominalPrescaler=8; can.Init.NominalSyncJumpWidth=2;
    can.Init.NominalTimeSeg1=13; can.Init.NominalTimeSeg2=2;
    can.Init.DataPrescaler=2; can.Init.DataSyncJumpWidth=2;
    can.Init.DataTimeSeg1=13; can.Init.DataTimeSeg2=2;
    can.Init.StdFiltersNbr=0; can.Init.ExtFiltersNbr=1;
    can.Init.TxFifoQueueMode=FDCAN_TX_FIFO_OPERATION;
    if(HAL_FDCAN_Init(&can)!=HAL_OK) return false;
    /* Provisional OSG bring-up protocol uses an extended-ID prefix.
     * Reject unrelated traffic in hardware so it cannot starve USB service. */
    FDCAN_FilterTypeDef filter={.IdType=FDCAN_EXTENDED_ID,.FilterIndex=0,
        .FilterType=FDCAN_FILTER_MASK,.FilterConfig=FDCAN_FILTER_TO_RXFIFO0,
        .FilterID1=OSG_GW_CAN_BASE,.FilterID2=OSG_GW_CAN_MASK};
    if(HAL_FDCAN_ConfigFilter(&can,&filter)!=HAL_OK ||
       HAL_FDCAN_ConfigGlobalFilter(&can,FDCAN_REJECT,FDCAN_REJECT,
           FDCAN_REJECT_REMOTE,FDCAN_REJECT_REMOTE)!=HAL_OK ||
       HAL_FDCAN_ConfigTxDelayCompensation(&can,28,0)!=HAL_OK ||
       HAL_FDCAN_EnableTxDelayCompensation(&can)!=HAL_OK || HAL_FDCAN_Start(&can)!=HAL_OK) return false;
    HAL_GPIO_WritePin(GPIOB,GPIO_PIN_5,GPIO_PIN_RESET);
    return true;
}

void board_init(void) {
    HAL_Init();
    __HAL_RCC_PWR_CLK_ENABLE();
    check(HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1));
    RCC_OscInitTypeDef osc={0};
    osc.OscillatorType=RCC_OSCILLATORTYPE_HSI|RCC_OSCILLATORTYPE_HSI48;
    osc.HSIState=RCC_HSI_ON; osc.HSIDiv=RCC_HSI_DIV1;
    osc.HSICalibrationValue=RCC_HSICALIBRATION_DEFAULT; osc.HSI48State=RCC_HSI48_ON;
    osc.PLL.PLLState=RCC_PLL_ON; osc.PLL.PLLSource=RCC_PLLSOURCE_HSI;
    osc.PLL.PLLM=RCC_PLLM_DIV1; osc.PLL.PLLN=8;
    osc.PLL.PLLP=RCC_PLLP_DIV2; osc.PLL.PLLQ=RCC_PLLQ_DIV2; osc.PLL.PLLR=RCC_PLLR_DIV2;
    check(HAL_RCC_OscConfig(&osc));
    RCC_ClkInitTypeDef clk={0};
    clk.ClockType=RCC_CLOCKTYPE_SYSCLK|RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_PCLK1;
    clk.SYSCLKSource=RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider=RCC_SYSCLK_DIV1; clk.APB1CLKDivider=RCC_HCLK_DIV1;
    check(HAL_RCC_ClockConfig(&clk,FLASH_LATENCY_2));
    RCC_PeriphCLKInitTypeDef peripherals={0};
    peripherals.PeriphClockSelection=RCC_PERIPHCLK_USB|RCC_PERIPHCLK_FDCAN;
    peripherals.UsbClockSelection=RCC_USBCLKSOURCE_HSI48;
    peripherals.FdcanClockSelection=RCC_FDCANCLKSOURCE_PCLK1;
    check(HAL_RCCEx_PeriphCLKConfig(&peripherals));
    __HAL_RCC_CRS_CLK_ENABLE();
    RCC_CRSInitTypeDef crs={0};
    crs.Prescaler=RCC_CRS_SYNC_DIV1; crs.Source=RCC_CRS_SYNC_SOURCE_USB;
    crs.Polarity=RCC_CRS_SYNC_POLARITY_RISING;
    crs.ReloadValue=__HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000,1000);
    crs.ErrorLimitValue=34; crs.HSI48CalibrationValue=32;
    HAL_RCCEx_CRSConfig(&crs);
    __HAL_RCC_GPIOA_CLK_ENABLE(); __HAL_RCC_GPIOB_CLK_ENABLE(); __HAL_RCC_SYSCFG_CLK_ENABLE();
    HAL_SYSCFG_DisableRemap(SYSCFG_REMAP_PA11|SYSCFG_REMAP_PA12);
    HAL_SYSCFG_StrobeDBattpinsConfig(SYSCFG_UCPD1_STROBE|SYSCFG_UCPD2_STROBE);
    HAL_GPIO_WritePin(GPIOB,GPIO_PIN_5,GPIO_PIN_SET);
    GPIO_InitTypeDef gpio={.Pin=GPIO_PIN_5,.Mode=GPIO_MODE_OUTPUT_PP,
        .Pull=GPIO_NOPULL,.Speed=GPIO_SPEED_FREQ_LOW};
    HAL_GPIO_Init(GPIOA,&gpio); HAL_GPIO_Init(GPIOB,&gpio); board_led(false);
    gpio.Pin=GPIO_PIN_0; gpio.Mode=GPIO_MODE_INPUT; HAL_GPIO_Init(GPIOB,&gpio);
    gpio.Pin=GPIO_PIN_8|GPIO_PIN_9; gpio.Mode=GPIO_MODE_AF_PP;
    gpio.Alternate=GPIO_AF3_FDCAN1; gpio.Speed=GPIO_SPEED_FREQ_HIGH; HAL_GPIO_Init(GPIOB,&gpio);
    gpio.Pin=GPIO_PIN_11|GPIO_PIN_12; gpio.Mode=GPIO_MODE_INPUT;
    gpio.Pull=GPIO_NOPULL; HAL_GPIO_Init(GPIOA,&gpio);
    __HAL_RCC_USB_CLK_ENABLE(); HAL_PWREx_EnableVddUSB();
    NVIC_SetPriority(USB_UCPD1_2_IRQn,1); NVIC_EnableIRQ(USB_UCPD1_2_IRQn);
    __HAL_RCC_FDCAN_CLK_ENABLE();
    if(!can_start()) board_fault();
}

bool board_can_receive(board_can_frame_t *frame) {
    if(!frame || recovering || !HAL_FDCAN_GetRxFifoFillLevel(&can,FDCAN_RX_FIFO0)) return false;
    FDCAN_RxHeaderTypeDef rx;
    if(HAL_FDCAN_GetRxMessage(&can,FDCAN_RX_FIFO0,&rx,frame->data)!=HAL_OK) {
        diagnostics.rx_dropped++; return false;
    }
    if(rx.IdType!=FDCAN_EXTENDED_ID || rx.RxFrameType!=FDCAN_DATA_FRAME ||
       rx.FDFormat!=FDCAN_FD_CAN || rx.BitRateSwitch!=FDCAN_BRS_ON) {
        diagnostics.rx_dropped++; return false;
    }
    frame->id=rx.Identifier; frame->size=dlc_sizes[(rx.DataLength>>16)&15U];
    diagnostics.rx++; return true;
}
bool board_can_enqueue(const board_can_frame_t *frame) {
    if(!frame || frame->id>0x1FFFFFFFU || frame->size>64 || tx_count==TX_QUEUE_CAPACITY) {
        diagnostics.tx_dropped++; return false;
    }
    tx_queue[(tx_head+tx_count)%TX_QUEUE_CAPACITY]=*frame;
    tx_count++; return true;
}
static void tx_pop(void) { tx_head=(uint8_t)((tx_head+1)%TX_QUEUE_CAPACITY); tx_count--; tx_attempts=0; }
static void tx_failed(uint32_t now) {
    diagnostics.tx_failed++;
    if(tx_attempts>=3) { diagnostics.tx_dropped++; tx_pop(); }
    tx_retry_at=now+TX_RETRY_MS;
}
static void enter_recovery(uint32_t now) {
    HAL_GPIO_WritePin(GPIOB,GPIO_PIN_5,GPIO_PIN_SET);
    (void)HAL_FDCAN_Stop(&can); recovering=true; recovery_at=now+CAN_RECOVERY_MS;
    diagnostics.tx_dropped+=tx_count; tx_head=tx_count=tx_attempts=0; tx_inflight=false;
}
void board_can_task(uint32_t now) {
    FDCAN_ProtocolStatusTypeDef status={0};
    (void)HAL_FDCAN_GetProtocolStatus(&can,&status);
    diagnostics.bus_off=status.BusOff!=0;
    diagnostics.hal_error=HAL_FDCAN_GetError(&can);
    if(can.Instance->IR&FDCAN_FLAG_RX_FIFO0_MESSAGE_LOST) {
        diagnostics.rx_dropped++; __HAL_FDCAN_CLEAR_FLAG(&can,FDCAN_FLAG_RX_FIFO0_MESSAGE_LOST);
    }
    if(status.BusOff && !recovering) enter_recovery(now);
    if(recovering) {
        if((int32_t)(now-recovery_at)>=0) {
            (void)HAL_FDCAN_DeInit(&can);
            if(can_start()) { recovering=false; diagnostics.recoveries++; }
            else recovery_at=now+CAN_RECOVERY_MS;
        }
        return;
    }
    if(tx_inflight) {
        if(!HAL_FDCAN_IsTxBufferMessagePending(&can,tx_buffer)) {
            tx_inflight=false;
            if(can.Instance->TXBTO&tx_buffer) { diagnostics.tx++; tx_pop(); }
            else tx_failed(now);
        } else if((uint32_t)(now-tx_started)>=TX_TIMEOUT_MS) {
            (void)HAL_FDCAN_AbortTxRequest(&can,tx_buffer);
            /* Wait for hardware cancellation before recycling the queue head. */
            if(!HAL_FDCAN_IsTxBufferMessagePending(&can,tx_buffer)) {
                tx_inflight=false; tx_failed(now);
            } else if((uint32_t)(now-tx_started)>=2U*TX_TIMEOUT_MS) {
                diagnostics.tx_failed++; enter_recovery(now);
            }
        }
        return;
    }
    if(!tx_count || (int32_t)(now-tx_retry_at)<0 || !HAL_FDCAN_GetTxFifoFreeLevel(&can)) return;
    const board_can_frame_t *frame=&tx_queue[tx_head];
    uint8_t dlc=0; while(dlc_sizes[dlc]<frame->size) dlc++;
    uint8_t data[64]={0}; memcpy(data,frame->data,frame->size);
    FDCAN_TxHeaderTypeDef tx={.Identifier=frame->id,.IdType=FDCAN_EXTENDED_ID,
        .TxFrameType=FDCAN_DATA_FRAME,.DataLength=(uint32_t)dlc<<16,
        .ErrorStateIndicator=FDCAN_ESI_ACTIVE,.BitRateSwitch=FDCAN_BRS_ON,
        .FDFormat=FDCAN_FD_CAN,.TxEventFifoControl=FDCAN_NO_TX_EVENTS};
    tx_attempts++;
    if(HAL_FDCAN_AddMessageToTxFifoQ(&can,&tx,data)!=HAL_OK) { tx_failed(now); return; }
    tx_buffer=HAL_FDCAN_GetLatestTxFifoQRequestBuffer(&can); tx_started=now; tx_inflight=true;
}
board_can_diagnostics_t board_can_diagnostics(void) {
    FDCAN_ErrorCountersTypeDef counters={0};
    (void)HAL_FDCAN_GetErrorCounters(&can,&counters);
    diagnostics.tx_queue=tx_count; diagnostics.rx_error=(uint8_t)counters.RxErrorCnt;
    diagnostics.tx_error=(uint8_t)counters.TxErrorCnt; diagnostics.recovering=recovering;
    return diagnostics;
}
void SysTick_Handler(void) { HAL_IncTick(); }
void USB_UCPD1_2_IRQHandler(void) { tusb_int_handler(0,true); }
uint32_t tusb_time_millis_api(void) { return HAL_GetTick(); }
void _init(void) { }
