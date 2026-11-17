/* Configuracao da biblioteca SSD1306 (afiskon/stm32-ssd1306, licenca MIT) para a Blue Pill */
#ifndef __SSD1306_CONF_H__
#define __SSD1306_CONF_H__

#define STM32F1
#define SSD1306_USE_I2C
#define SSD1306_I2C_PORT        hi2c2   /* I2C2: PB10 = SCL, PB11 = SDA */
#define SSD1306_I2C_ADDR        (0x3C << 1)   /* se a tela nao acender, troque por (0x3D << 1) */

#define SSD1306_INCLUDE_FONT_6x8
#define SSD1306_INCLUDE_FONT_7x10
#define SSD1306_INCLUDE_FONT_11x18
#define SSD1306_INCLUDE_FONT_16x26

#endif
