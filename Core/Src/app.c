/*
 * Aplicacao: menu do encoder, tela OLED e LED de "estou vivo".
 * O audio, as teclas e o encoder rodam na interrupcao do DMA; aqui fica so a parte lenta.
 */
#include "app.h"
#include "audio.h"
#include "input.h"
#include "rtttl.h"
#include "midi.h"
#include "cmd.h"
#include "ssd1306.h"
#include "ssd1306_fonts.h"
#include "main.h"
#include <stdio.h>
#include <string.h>

extern TIM_HandleTypeDef htim1;          /* PWM de audio (PA8 / PA9) */
extern DMA_HandleTypeDef hdma_tim1_up;   /* DMA1 canal 5 */
extern UART_HandleTypeDef huart2;        /* HC-05 */

typedef enum { MODO_TECLADO, MODO_MENU, MODO_EDITAR } Modo;

static const char *NOMES_CROM[12] = {"Do", "Do#", "Re", "Re#", "Mi", "Fa", "Fa#", "Sol", "Sol#", "La", "La#", "Si"};
static const char *ITENS[] = {"Volume", "Timbre", "Musica", "Estereo", "HC-05 115200", "Sair"};
#define NUM_ITENS 6
#define TELA_MIN_MS_MUSICA 150

static Modo modo = MODO_TECLADO;
static int item = 0;

/* "assinatura" do que esta na tela: so redesenha quando muda */
static uint16_t d_mascara = 0xFFFF;
static int d_oit = -1, d_timbre = -1, d_modo = -1, d_item = -1, d_vol = -1, d_tocando = -1, d_est = -1, d_meme = -1;
static uint32_t d_versao = 0xFFFFFFFF, d_meme_ver = 0xFFFFFFFF, d_seg = 0xFFFFFFFF;
static uint32_t ultima_tela = 0, ultimo_led = 0;

/* ---------- Desenho ---------- */
static void texto(uint8_t x, uint8_t y, const char *s, SSD1306_Font_t f, SSD1306_COLOR c) {
  char b[24];
  strncpy(b, s, sizeof(b) - 1);
  b[sizeof(b) - 1] = 0;
  ssd1306_SetCursor(x, y);
  ssd1306_WriteString(b, f, c);
}

static void nome_nota(int midi, char *out, size_t tam) {
  snprintf(out, tam, "%s%d", NOMES_CROM[((midi % 12) + 12) % 12], midi / 12 - 1);
}

static int meme_ativo(void) {
  return cmd_meme_versao() != 0 && (int32_t)(cmd_meme_ate() - HAL_GetTick()) > 0;
}

static void desenhar_meme(void) {
  const char *s = cmd_meme_texto();
  char ln[19];
  uint8_t y = 2;
  while (*s && y <= 52) {                       /* quebra em linhas de 18 caracteres */
    while (*s == ' ') s++;
    size_t n = strlen(s);
    if (n > 18) {
      n = 18;
      for (size_t k = 18; k > 6; k--) if (s[k] == ' ') { n = k; break; }
    }
    memcpy(ln, s, n);
    ln[n] = 0;
    texto(0, y, ln, Font_7x10, White);
    s += n;
    y += 12;
  }
}

static void desenhar_menu(void) {
  char b[24];
  texto(0, 0, "MENU", Font_6x8, White);
  for (int i = 0; i < NUM_ITENS; i++) {
    uint8_t y = (uint8_t)(11 + i * 9);
    int sel = (i == item);
    const char *m = sel ? "> " : "  ";
    if (i == 0)      snprintf(b, sizeof(b), "%s%s: %d%%", m, ITENS[i], audio_get_volume_pct());
    else if (i == 1) snprintf(b, sizeof(b), "%s%s: %s", m, ITENS[i], audio_nome_timbre(audio_get_timbre()));
    else if (i == 3) snprintf(b, sizeof(b), "%s%s: %s", m, ITENS[i], audio_get_estereo() ? "on" : "off");
    else             snprintf(b, sizeof(b), "%s%s", m, ITENS[i]);
    if (sel && modo == MODO_EDITAR) {
      ssd1306_FillRectangle(0, (uint8_t)(y - 1), 127, (uint8_t)(y + 8), White);
      texto(0, y, b, Font_6x8, Black);
    } else {
      texto(0, y, b, Font_6x8, White);
    }
  }
}

static void desenhar(uint16_t mascara) {
  char b[40];
  ssd1306_Fill(Black);

  if (meme_ativo()) {
    desenhar_meme();
  } else if (midi_tocando()) {
    snprintf(b, sizeof(b), "%.12s MIDI o%d", midi_nome(), audio_get_oitava());
    texto(0, 0, b, Font_6x8, White);
    texto(0, 16, midi_nota_atual(), Font_16x26, White);
    snprintf(b, sizeof(b), "%lus / %lus", (unsigned long)(midi_posicao_ms() / 1000), (unsigned long)(midi_duracao_ms() / 1000));
    texto(0, 56, b, Font_6x8, White);
  } else if (rtttl_tocando()) {
    snprintf(b, sizeof(b), "%.15s o%d", rtttl_nome(), audio_get_oitava());
    texto(0, 0, b, Font_6x8, White);
    texto(0, 16, rtttl_nota_atual(), Font_16x26, White);
    texto(0, 56, rtttl_historico(), Font_6x8, White);
  } else if (modo != MODO_TECLADO) {
    desenhar_menu();
  } else {
    snprintf(b, sizeof(b), "Oit %d | %s", audio_get_oitava(), audio_nome_timbre(audio_get_timbre()));
    texto(0, 0, b, Font_6x8, White);
    int qtd = 0, ultima = 0;
    for (int i = 0; i < NTECLAS; i++) if (mascara & (1u << i)) { qtd++; ultima = i; }
    if (qtd == 0) {
      texto(0, 20, "--", Font_16x26, White);
    } else if (qtd == 1) {
      int m = audio_tecla_nota_real(ultima);
      nome_nota(m, b, sizeof(b));
      texto(0, 18, b, Font_16x26, White);
      snprintf(b, sizeof(b), "%lu Hz", (unsigned long)audio_freq_hz_midi(m));
      texto(0, 56, b, Font_6x8, White);
    } else {
      char nm[12];
      b[0] = 0;
      for (int i = 0; i < NTECLAS; i++) {
        if (!(mascara & (1u << i))) continue;
        nome_nota(audio_tecla_nota_real(i), nm, sizeof(nm));
        if (b[0]) strncat(b, "+", sizeof(b) - strlen(b) - 1);
        strncat(b, nm, sizeof(b) - strlen(b) - 1);
      }
      size_t n = strlen(b);
      if (n <= 11) texto(0, 22, b, Font_11x18, White);
      else if (n <= 18) texto(0, 24, b, Font_7x10, White);
      else { texto(0, 20, b, Font_6x8, White); if (n > 21) texto(0, 30, b + 21, Font_6x8, White); }
    }
  }
  ssd1306_UpdateScreen();
}

/* ---------- Configurar o HC-05 (ele precisa estar em modo AT) ---------- */
static void configurar_hc05(void) {
  static char resp[96];
  ssd1306_Fill(Black);
  texto(0, 0, "Configurando HC-05", Font_6x8, White);
  texto(0, 12, "aguarde...", Font_6x8, White);
  ssd1306_UpdateScreen();

  bool ok = cmd_hc05_115200(resp, sizeof(resp));

  ssd1306_Fill(Black);
  texto(0, 0, ok ? "HC-05 em 115200!" : "Falhou:", Font_7x10, White);
  for (int l = 0; l < 4; l++) {                     /* resposta do modulo, 21 letras por linha */
    size_t ini = (size_t)l * 21;
    if (ini >= strlen(resp)) break;
    char ln[22];
    strncpy(ln, resp + ini, 21);
    ln[21] = 0;
    texto(0, (uint8_t)(16 + l * 10), ln, Font_6x8, White);
  }
  if (ok) texto(0, 56, "Desligue e ligue", Font_6x8, White);
  ssd1306_UpdateScreen();
  HAL_Delay(4000);
  d_modo = -1;                                      /* forca redesenhar a tela */
}

/* ---------- Inicializacao ---------- */
void app_init(void) {
  input_init();
  ssd1306_Init();
  audio_init(&htim1, &hdma_tim1_up);
  cmd_init(&huart2);
  desenhar(0);
  cmd_println("");
  cmd_println("Mini teclado pronto! Digite help.");
}

/* ---------- Loop principal ---------- */
void app_loop(void) {
  cmd_atualizar();
  rtttl_atualizar();
  midi_atualizar();

  int giro = input_encoder_delta();
  int clique = input_sw_clique();
  int tocando = rtttl_tocando() || midi_tocando();

  if (tocando) {
    if (giro) audio_set_oitava(audio_get_oitava() + giro);     /* transpoe a musica */
    if (clique) { rtttl_parar(); midi_parar(); }
  } else if (modo == MODO_TECLADO) {
    if (giro) audio_set_oitava(audio_get_oitava() + giro);
    if (clique) { modo = MODO_MENU; item = 0; }
  } else if (modo == MODO_MENU) {
    if (giro) item = (item + giro + NUM_ITENS * 4) % NUM_ITENS;
    if (clique) {
      if (item == 0 || item == 1) modo = MODO_EDITAR;
      else if (item == 2) {
        modo = MODO_TECLADO;
        if (midi_tem_guardada()) midi_tocar();
        else rtttl_iniciar(rtttl_guardada(), false);
      } else if (item == 3) audio_set_estereo(!audio_get_estereo());
      else if (item == 4) configurar_hc05();
      else modo = MODO_TECLADO;
    }
  } else {
    if (giro) {
      if (item == 0) audio_set_volume_pct(audio_get_volume_pct() + giro * 5);
      else audio_set_timbre((audio_get_timbre() + giro + NTIMBRES * 4) % NTIMBRES);
    }
    if (clique) modo = MODO_MENU;
  }

  /* LED da placa pisca: o programa esta rodando */
  if (HAL_GetTick() - ultimo_led >= 500) {
    ultimo_led = HAL_GetTick();
    HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
  }

  /* So redesenha quando algo mudou (durante a musica, no maximo a cada 150 ms) */
  tocando = rtttl_tocando() || midi_tocando();
  uint16_t mascara = input_teclas();
  uint32_t versao = rtttl_versao() + midi_versao();
  uint32_t seg = midi_tocando() ? midi_posicao_ms() / 1000 : 0;
  int mm = meme_ativo();
  int mudou = mascara != d_mascara || audio_get_oitava() != d_oit || audio_get_timbre() != d_timbre ||
              (int)modo != d_modo || item != d_item || audio_get_volume_pct() != d_vol ||
              tocando != d_tocando || versao != d_versao || audio_get_estereo() != d_est ||
              mm != d_meme || cmd_meme_versao() != d_meme_ver || seg != d_seg;
  int pode = !tocando || (HAL_GetTick() - ultima_tela >= TELA_MIN_MS_MUSICA);

  if (mudou && pode) {
    desenhar(mascara);
    d_mascara = mascara; d_oit = audio_get_oitava(); d_timbre = audio_get_timbre();
    d_modo = (int)modo; d_item = item; d_vol = audio_get_volume_pct(); d_tocando = tocando;
    d_versao = versao; d_est = audio_get_estereo(); d_meme = mm; d_meme_ver = cmd_meme_versao(); d_seg = seg;
    ultima_tela = HAL_GetTick();
  }
}
