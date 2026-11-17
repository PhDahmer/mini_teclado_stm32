# Mini teclado musical - STM32F103C8T6 (Blue Pill)

10 teclas, encoder com menu, tela OLED, audio estereo por PWM com DMA e comandos/musicas pelo Bluetooth (HC-05).

## 1. Ligacoes

| Peca | Pino da peca | Blue Pill |
|---|---|---|
| Tela OLED (I2C2) | VCC / GND | 3.3 / G |
| | SCL / SDA | PB10 / PB11 |
| Teclas 1..10 (Do4 Re4 Mi4 Fa4 Sol4 La4 Si4 Do5 Re5 Mi5) | um lado | PB9, PB8, PB7, PB6, PB5, PB4, PB3, PA15, PA12, PA10 (coluna da esquerda, de baixo para cima, USB para cima) |
| | outro lado | G (GND) |
| Encoder KY-040 | + / GND | 3.3 / G |
| | CLK / DT / SW | PA4 / PA5 / PA6 |
| HC-05 | VCC / GND | 5V / G (o 5V so existe com o USB da placa ligado ou pelo pino 5V do ST-Link) |
| | TXD | PA3 (RX da USART2) |
| | RXD | PA2 (TX da USART2) |
| Audio esquerdo | | PA8 (TIM1_CH1) -> filtro -> ponta do P3 |
| Audio direito | | PA9 (TIM1_CH2) -> filtro -> ring 1 do P3 |
| P3 | ring 2 e corpo | G |
| ST-Link | 3.3 / SWDIO / SWCLK / GND | os 4 pinos da ponta da placa |

Filtro de cada canal (com as pecas atuais): pino -> resistor 2,2 kOhm -> capacitor eletrolitico 10 uF
(+ do lado do resistor) -> P3. Sem capacitor para o GND (o de 100 nF corta os agudos com 2,2 kOhm).
Melhor ainda, quando comprar: 10 nF (103) do ponto entre o resistor e o eletrolitico ate o GND, em cada canal.

## 2. CubeMX (pode partir do projeto do professor)

1. **SYS**: Debug = *Serial Wire* (obrigatorio para nao perder o ST-Link). Timebase = SysTick.
2. **RCC**: HSE = *Crystal/Ceramic Resonator*.
3. **Clock Configuration**: HSE 8 MHz -> PLL Source HSE -> PLLMul x9 -> System Clock Mux PLLCLK
   -> HCLK 72 MHz, APB1 /2, APB2 /1 (igual ao projeto do professor).
4. **GPIO** (clicar no pino no desenho do chip):
   - PC13: GPIO_Output, User Label `LED`.
   - PB3, PB4, PB5, PB6, PB7, PB8, PB9, PA10, PA12, PA15, PA4, PA5, PA6: GPIO_Input.
     Em System Core > GPIO, selecione cada um e coloque **Pull-up**.
     (PB3, PB4 e PA15 so ficam livres com o Debug em Serial Wire, passo 1.)
5. **TIM1** (Timers > TIM1):
   - Clock Source: Internal Clock. Channel1: PWM Generation CH1. Channel2: PWM Generation CH2.
   - Parameter Settings: Prescaler 0, Counter Mode Up, Counter Period **511**,
     Repetition Counter **6**, auto-reload preload **Enable**.
   - PWM Generation Channel 1 e 2: PWM mode 1, Pulse 256, Output compare preload Enable, CH Polarity High.
   - DMA Settings > Add: **TIM1_UP** (DMA1 Channel 5), Memory To Peripheral, Priority Very High,
     Mode **Circular**, Increment Address so em Memory, Data Width **Half Word / Half Word**.
   - Confira que os pinos ficaram PA8 e PA9.
6. **USART2** (Connectivity): Mode Asynchronous, 9600 Bits/s, 8 bits, None, 1 stop.
   - DMA Settings > Add **USART2_RX** (DMA1 Channel 6): Peripheral To Memory, Mode **Circular**, Byte/Byte.
   - DMA Settings > Add **USART2_TX** (DMA1 Channel 7): Memory To Peripheral, Mode Normal, Byte/Byte.
   - NVIC Settings: marque **USART2 global interrupt**.
7. **I2C2** (Connectivity): I2C, Speed Mode **Fast Mode** (400 kHz). Pinos PB10 (SCL) / PB11 (SDA).
8. **NVIC** (System Core): DMA1 channel5 com Preemption Priority **0**; DMA1 channel6, channel7
   e USART2 com **5**. SysTick fica como esta.
9. **Project Manager**: Toolchain STM32CubeIDE. Linker Settings: Minimum Heap **0x200**,
   Minimum Stack Size **0x800**. Generate Code.

A pasta `referencia_cubemx` tem um exemplo do que o CubeMX deve gerar. Nao copie esses arquivos:
use so para comparar se algo nao funcionar.

## 3. STM32CubeIDE

1. Copie `Core/Inc/*.h` para `Core/Inc` do projeto e `Core/Src/*.c` para `Core/Src`.
   Clique com o botao direito no projeto > Refresh (F5).
2. No `main.c`, so dentro dos blocos USER CODE:
   ```c
   /* USER CODE BEGIN Includes */
   #include "app.h"
   /* USER CODE END Includes */

   /* USER CODE BEGIN 2 */
   app_init();
   /* USER CODE END 2 */

   while (1)
   {
     /* USER CODE END WHILE */

     /* USER CODE BEGIN 3 */
     app_loop();
   }
   /* USER CODE END 3 */
   ```
3. Build (martelo) e Run/Debug com o ST-Link como no projeto do professor.
4. Se aparecer `region FLASH overflowed`: Project > Properties > C/C++ Build > Settings >
   MCU GCC Compiler > Optimization: **Optimize for size (-Os)**. O audio ja usa -O2 sozinho
   (`#pragma` no comeco do audio.c).

A ordem das notas nas teclas pode ser trocada sem recompilar: `tecla 1 C5`, `tecla 10 do4` etc.

## 4. Bluetooth (celular Android)

App **Serial Bluetooth Terminal**. Pareie o HC-05 (senha 1234 ou 0000), conecte em
Devices > Bluetooth Classic. Em Settings > Send, deixe Newline = CR+LF. Digite `help`.

| Comando | O que faz |
|---|---|
| `nome:d=4,o=5,b=120:notas` | toca uma musica RTTTL (uma linha) |
| `storms`, `tocar`, `stop` | musica pronta, repete a ultima guardada, para |
| `bpm 140`, `bpm auto` | forca o andamento das RTTTL |
| `volume 0-100`, `oitava 2-7`, `estereo on/off` | ajustes |
| `timbre piano` (ou numero) | escolhe o timbre |
| `tecla`, `tecla 8 C5`, `tecla 3 sol#4` | lista / troca a nota de uma tecla |
| `inst piano`, `inst piano ataque 30` | mostra / muda ataque, decai, sustenta, solta, vibrato, brilho, onda |
| `harm flauta 100,40,10` | cria uma nova forma de onda (ate 12 harmonicos) |
| `tela 5 texto` | mostra um texto na tela por 5 s |
| `cpu` | quanto da CPU o audio esta usando |
| `reiniciar` | reinicia a placa (tudo volta ao padrao) |

Nada fica salvo: ao reiniciar, tudo volta ao padrao.

## 5. MIDI (pelo computador)

Pareie o HC-05 no Windows; ele vira uma porta COM (Gerenciador de Dispositivos > Portas).
```
pip install mido pyserial
python enviar_midi.py musica.mid COM5
```
A musica fica guardada na RAM (ate 1600 eventos, 8 notas ao mesmo tempo) e pode ser repetida com
`tocar` ou pelo menu. Cada canal MIDI usa um timbre proximo do instrumento original.

## 6. Como funciona (resumo para o relatorio)

- **TIM1** gera o PWM de 9 bits a 140,6 kHz em PA8 e PA9. O contador de repeticao (RCR = 6) gera
  um evento de update a cada 7 periodos = **20089 amostras por segundo**.
- Cada update dispara o **DMA1 canal 5** em modo *burst* (TIM1->DCR/DMAR), que grava CCR1 e CCR2
  sozinho a partir de um buffer circular na RAM. Quando metade do buffer termina, a interrupcao do
  DMA calcula as proximas 64 amostras (double buffering).
- O calculo soma ate 18 vozes (10 teclas + 8 da musica): tabela de onda, envelope ADSR, vibrato,
  filtro e panorama estereo, so com inteiros.
- Os botoes e o encoder sao lidos dentro dessa interrupcao (debounce por contagem).
- **USART2** com **DMA circular** na recepcao (DMA1 canal 6) e DMA no envio (DMA1 canal 7).
- O loop principal so cuida dos comandos, da musica (agendada pelo HAL_GetTick, sem atraso
  acumulado) e da tela OLED (I2C2).
