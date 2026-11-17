#ifndef APP_H
#define APP_H
void app_init(void);   /* chamar uma vez depois dos MX_..._Init() */
void app_loop(void);   /* chamar dentro do while (1) */
#endif
