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
static const char *ITENS[] = {"Volume", "Timbre", "Musica", "Estereo", "Config HC-05", "Sair"};
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

/* ---------- Menu ---------- */
#define MENU_LINHAS 4      /* itens visiveis de uma vez */
#define MENU_Y0     13     /* onde comeca a lista (abaixo da barra de titulo) */
#define MENU_ALT    12     /* altura de cada linha */
static int menu_primeiro = 0;   /* primeiro item visivel (a lista rola) */

/* barra de titulo invertida (fundo branco, letra preta), com um texto opcional na direita */
static void barra_titulo(const char *titulo, const char *direita) {
  ssd1306_FillRectangle(0, 0, 127, 10, White);
  texto(3, 2, titulo, Font_6x8, Black);
  if (direita) texto((uint8_t)(125 - 6 * strlen(direita)), 2, direita, Font_6x8, Black);
}

/* texto centralizado na tela */
static void texto_centro(uint8_t y, const char *s, SSD1306_Font_t f, SSD1306_COLOR c) {
  int x = (128 - (int)strlen(s) * f.width) / 2;
  texto((uint8_t)(x < 0 ? 0 : x), y, s, f, c);
}

/* valor que aparece na direita de cada item */
static void valor_item(int i, char *v, size_t tam) {
  v[0] = 0;
  if (i == 0) snprintf(v, tam, "%d%%", audio_get_volume_pct());
  else if (i == 1) snprintf(v, tam, "%s", audio_nome_timbre(audio_get_timbre()));
  else if (i == 2) {
    const char *nome = midi_tem_guardada() ? midi_nome() : rtttl_guardada();
    size_t n = 0;
    while (nome[n] && nome[n] != ':' && n < 7) n++;       /* RTTTL: o nome vai ate o ':' */
    memcpy(v, nome, n);
    v[n] = 0;
  }
  else if (i == 3) snprintf(v, tam, "%s", audio_get_estereo() ? "on" : "off");
  else snprintf(v, tam, ">");
}

static void desenhar_lista(void) {
  char b[24], v[16];
  if (item < menu_primeiro) menu_primeiro = item;
  if (item >= menu_primeiro + MENU_LINHAS) menu_primeiro = item - MENU_LINHAS + 1;
  snprintf(b, sizeof(b), "%d/%d", item + 1, NUM_ITENS);
  barra_titulo("MENU", b);

  for (int l = 0; l < MENU_LINHAS; l++) {
    int i = menu_primeiro + l;
    uint8_t y = (uint8_t)(MENU_Y0 + l * MENU_ALT);
    SSD1306_COLOR cor = White;
    if (i == item) {                               /* item escolhido: faixa branca com cantos arredondados */
      ssd1306_FillRectangle(0, y, 121, (uint8_t)(y + MENU_ALT - 1), White);
      ssd1306_DrawPixel(0, y, Black);   ssd1306_DrawPixel(121, y, Black);
      ssd1306_DrawPixel(0, (uint8_t)(y + MENU_ALT - 1), Black);
      ssd1306_DrawPixel(121, (uint8_t)(y + MENU_ALT - 1), Black);
      cor = Black;
    }
    texto(3, (uint8_t)(y + 1), ITENS[i], Font_7x10, cor);
    valor_item(i, v, sizeof(v));
    texto((uint8_t)(119 - 7 * strlen(v)), (uint8_t)(y + 1), v, Font_7x10, cor);
  }

  /* barra de rolagem na direita */
  const int alt = MENU_LINHAS * MENU_ALT;                      /* 48 px */
  const int pol = alt * MENU_LINHAS / NUM_ITENS;               /* tamanho do "polegar" */
  const int py = MENU_Y0 + (alt - pol) * menu_primeiro / (NUM_ITENS - MENU_LINHAS);
  for (int y = MENU_Y0; y < MENU_Y0 + alt; y += 2) ssd1306_DrawPixel(125, (uint8_t)y, White);
  ssd1306_FillRectangle(124, (uint8_t)py, 126, (uint8_t)(py + pol - 1), White);
}

/* tela de ajuste do volume: numero grande + barra */
static void desenhar_editar_volume(void) {
  char b[16];
  int v = audio_get_volume_pct();
  barra_titulo("VOLUME", NULL);
  snprintf(b, sizeof(b), "%d%%", v);
  texto_centro(17, b, Font_16x26, White);
  ssd1306_DrawRectangle(4, 50, 123, 60, White);
  if (v > 0) ssd1306_FillRectangle(6, 52, (uint8_t)(6 + 115 * v / 100), 58, White);
}

/* tela de escolha do timbre: nome grande, setas e uma bolinha por timbre */
static void desenhar_editar_timbre(void) {
  char b[24];
  int t = audio_get_timbre();
  snprintf(b, sizeof(b), "%d/%d", t + 1, NTIMBRES);
  barra_titulo("TIMBRE", b);
  texto(0, 24, "<", Font_7x10, White);
  texto(121, 24, ">", Font_7x10, White);
  texto_centro(20, audio_nome_timbre(t), Font_11x18, White);
  const int passo = 120 / NTIMBRES, x0 = (128 - passo * (NTIMBRES - 1)) / 2;
  for (int i = 0; i < NTIMBRES; i++) {
    uint8_t x = (uint8_t)(x0 + i * passo);
    if (i == t) ssd1306_FillCircle(x, 54, 2, White);
    else ssd1306_DrawPixel(x, 54, White);
  }
}

static void desenhar_menu(void) {
  if (modo == MODO_EDITAR && item == 0) desenhar_editar_volume();
  else if (modo == MODO_EDITAR && item == 1) desenhar_editar_timbre();
  else desenhar_lista();
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
  midi_init();
  desenhar(0);
  cmd_println("");
  cmd_println("Mini teclado pronto! Digite help.");
  if (midi_tem_guardada()) {
    char b[48];
    snprintf(b, sizeof(b), "MIDI guardado: %s (digite tocar)", midi_nome());
    cmd_println(b);
  }
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
