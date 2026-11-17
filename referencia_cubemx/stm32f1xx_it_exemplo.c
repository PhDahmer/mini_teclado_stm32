#include "main.h"
extern DMA_HandleTypeDef hdma_tim1_up, hdma_usart2_rx, hdma_usart2_tx;
extern UART_HandleTypeDef huart2;
void NMI_Handler(void) { while (1) {} }
void HardFault_Handler(void) { while (1) {} }
void MemManage_Handler(void) { while (1) {} }
void BusFault_Handler(void) { while (1) {} }
void UsageFault_Handler(void) { while (1) {} }
void SVC_Handler(void) {}
void DebugMon_Handler(void) {}
void PendSV_Handler(void) {}
void SysTick_Handler(void) { HAL_IncTick(); }
void DMA1_Channel5_IRQHandler(void) { HAL_DMA_IRQHandler(&hdma_tim1_up); }
void DMA1_Channel6_IRQHandler(void) { HAL_DMA_IRQHandler(&hdma_usart2_rx); }
void DMA1_Channel7_IRQHandler(void) { HAL_DMA_IRQHandler(&hdma_usart2_tx); }
void USART2_IRQHandler(void) { HAL_UART_IRQHandler(&huart2); }
