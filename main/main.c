#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/timer.h"
#include "hardware/clocks.h"

#include "tft_lcd_ili9341/ili9341/ili9341.h"
#include "tft_lcd_ili9341/gfx/gfx_ili9341.h"

#include "pinos.h"
#include "tom_verde.h"
#include "tom_vermelho.h"
#include "tom_azul.h"
#include "tom_amarelo.h"
#include "ganhou.h"
#include "perdeu.h"
#include "play.h"

// ── LCD ───────────────────────────────────────────────────────────────────────
#define SCREEN_ROTATION 1
#define SCREEN_W        320
#define SCREEN_H        240

// ── Sequência ────────────────────────────────────────────────────────────────
#define MAX_SEQ 5

static void sequence_init(int seq[2][MAX_SEQ]) {
    static const int fixed[2][MAX_SEQ] = {
        {0, 1, 2, 3, 0}, // P1
        {2, 3, 1, 0, 2}, // P2
    };
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < MAX_SEQ; i++) seq[p][i] = fixed[p][i];
}

// ── Áudio PWM ────────────────────────────────────────────────────────────────
#define AUDIO_SAMPLE_RATE 11000
#define PWM_WRAP          255

static const uint8_t  *COLOR_DATA[NUM_COLORS] = {VERDE_DATA, VERMELHO_DATA, AZUL_DATA, AMARELO_DATA};
static const uint32_t  COLOR_LEN[NUM_COLORS]  = {VERDE_DATA_LENGTH, VERMELHO_DATA_LENGTH, AZUL_DATA_LENGTH, AMARELO_DATA_LENGTH};

// Usado pela IRQ do timer de áudio — precisa ser global
typedef struct {
    const uint8_t    *buf;
    uint32_t          len;
    volatile uint32_t pos;
    volatile bool     done;
} AudioState;
static AudioState g_audio = {NULL, 0, 0, true};

static bool audio_timer_cb(struct repeating_timer *t) {
    (void)t;
    if (g_audio.done) return true;
    if (g_audio.pos < g_audio.len)
        pwm_set_gpio_level(BUZZER_PIN, g_audio.buf[g_audio.pos++]);
    else {
        pwm_set_gpio_level(BUZZER_PIN, 128);
        g_audio.done = true;
    }
    return true;
}

static struct repeating_timer audio_timer;

static void audio_init(void) {
    gpio_set_function(BUZZER_PIN, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(BUZZER_PIN);
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_wrap(&cfg, PWM_WRAP);
    pwm_config_set_clkdiv(&cfg, (float)clock_get_hz(clk_sys) / ((float)AUDIO_SAMPLE_RATE * (PWM_WRAP + 1)));
    pwm_init(slice, &cfg, true);
    pwm_set_gpio_level(BUZZER_PIN, 128);
    add_repeating_timer_us(-(1000000 / AUDIO_SAMPLE_RATE), audio_timer_cb, NULL, &audio_timer);
}

static void audio_play(const uint8_t *data, uint32_t len) {
    g_audio.done = true;
    g_audio.buf  = data;
    g_audio.len  = len;
    g_audio.pos  = 0;
    g_audio.done = false;
}

static void audio_wait(void) {
    while (!g_audio.done) tight_loop_contents();
}

// ── LCD helpers ──────────────────────────────────────────────────────────────
static void lcd_centered(int y, const char *msg, uint16_t color, uint8_t size) {
    gfx_setTextSize(size);
    gfx_setTextColor(color);
    int w = gfx_getTextWidth(msg);
    gfx_drawText((SCREEN_W - w) / 2, y, msg);
}

static void lcd_clear(void) {
    gfx_fillRect(0, 0, SCREEN_W, SCREEN_H, ILI9341_BLACK);
}

static void lcd_menu(int selected) {
    lcd_clear();
    lcd_centered(20, "GENIUS", ILI9341_GREEN, 4);
    // 1 jogador: destacado em verde se selecionado, branco se não
    uint16_t c1 = (selected == 1) ? ILI9341_GREEN  : ILI9341_WHITE;
    uint16_t c2 = (selected == 2) ? ILI9341_YELLOW : ILI9341_WHITE;
    lcd_centered(SCREEN_H / 2 - 10, "[Verde]  1 Jogador",  c1, 2);
    lcd_centered(SCREEN_H / 2 + 25, "[Verm.]  2 Jogadores", c2, 2);
    lcd_centered(SCREEN_H - 24, "PLAY para confirmar", ILI9341_WHITE, 1);
}

static void lcd_waiting(int player, int score, int phase) {
    lcd_clear();
    char who[16];
    snprintf(who, sizeof(who), "Vez do P%d!", player);
    lcd_centered(SCREEN_H / 2 - 20, who, player == 1 ? ILI9341_CYAN : ILI9341_YELLOW, 3);
    gfx_setTextSize(2);
    gfx_setTextColor(ILI9341_WHITE);
    char buf[32];
    snprintf(buf, sizeof(buf), "Fase:%d  Pts:%d", phase, score);
    gfx_drawText(8, SCREEN_H - 24, buf);
}

static void lcd_final(int s1, int s2, int nplayers) {
    lcd_clear();
    lcd_centered(SCREEN_H / 2 - 50, "GAME OVER", ILI9341_RED, 3);
    char buf[32];
    if (nplayers == 2) {
        snprintf(buf, sizeof(buf), "P1: %d", s1);
        lcd_centered(SCREEN_H / 2 + 0,  buf, ILI9341_CYAN,   2);
        snprintf(buf, sizeof(buf), "P2: %d", s2);
        lcd_centered(SCREEN_H / 2 + 30, buf, ILI9341_YELLOW, 2);
    } else {
        snprintf(buf, sizeof(buf), "Pontos: %d", s1);
        lcd_centered(SCREEN_H / 2 + 10, buf, ILI9341_WHITE, 2);
    }
}

static void lcd_win(int s1, int s2, int nplayers) {
    lcd_clear();
    lcd_centered(SCREEN_H / 2 - 50, "YOU WIN!", ILI9341_GREEN, 3);
    char buf[32];
    if (nplayers == 2) {
        snprintf(buf, sizeof(buf), "P1: %d", s1);
        lcd_centered(SCREEN_H / 2 + 0,  buf, ILI9341_CYAN,   2);
        snprintf(buf, sizeof(buf), "P2: %d", s2);
        lcd_centered(SCREEN_H / 2 + 30, buf, ILI9341_YELLOW, 2);
    } else {
        snprintf(buf, sizeof(buf), "Pontos: %d", s1);
        lcd_centered(SCREEN_H / 2 + 10, buf, ILI9341_WHITE, 2);
    }
}

// ── Timeout ──────────────────────────────────────────────────────────────────
#define INPUT_TIMEOUT_US (7 * 1000000ULL)
static void timeout_reset(uint64_t *start) { *start = time_us_64(); }
static bool timeout_expired(uint64_t start) { return (time_us_64() - start) >= INPUT_TIMEOUT_US; }

// ── Estados ──────────────────────────────────────────────────────────────────
typedef enum { ST_MENU, ST_IDLE, ST_SHOW, ST_INPUT, ST_WIN, ST_LOSE } State;

static volatile int  btn_color = -1;
static volatile bool btn_play  = false;
static State state = ST_MENU;

static int num_players = 1;  // 1 ou 2
static int menu_sel    = 1;  // seleção atual no menu

// Estado por jogador
static int phase[2]     = {1, 1};
static int input_idx[2] = {0, 0};
static int score[2]     = {0, 0};
static int cur = 0;

// ── LEDs ─────────────────────────────────────────────────────────────────────
static void led_on(int c)  { gpio_put(LED_PINS[c], 1); }
static void led_off(int c) { gpio_put(LED_PINS[c], 0); }
static void all_off(void)  { for (int i = 0; i < NUM_COLORS; i++) led_off(i); }

static void all_blink(int n) {
    for (int i = 0; i < n; i++) {
        for (int c = 0; c < NUM_COLORS; c++) led_on(c);
        sleep_ms(300); all_off(); sleep_ms(300);
    }
}

static void show_color(int c) {
    led_on(c);
    audio_play(COLOR_DATA[c], COLOR_LEN[c]);
    audio_wait();
    led_off(c);
    sleep_ms(80);
}

// ── ISR ──────────────────────────────────────────────────────────────────────
static void gpio_cb(uint gpio, uint32_t events) {
    if (!(events & GPIO_IRQ_EDGE_RISE)) return;
    if (gpio == PIN_BTN_PLAY_PAUSE) { btn_play = true;  return; }
    if (gpio == PIN_BTN_VERDE)      { btn_color = 0;    return; }
    if (gpio == PIN_BTN_VERMELHO)   { btn_color = 1;    return; }
    if (gpio == PIN_BTN_AMARELO)    { btn_color = 2;    return; }
    if (gpio == PIN_BTN_AZUL)       { btn_color = 3;    return; }
}

// ── Setup ────────────────────────────────────────────────────────────────────
static void setup(void) {
    stdio_init_all();
    LCD_initDisplay();
    LCD_setRotation(SCREEN_ROTATION);
    gfx_init();
    gfx_clear();
    audio_init();

    for (int i = 0; i < NUM_COLORS; i++) {
        gpio_init(LED_PINS[i]);
        gpio_set_dir(LED_PINS[i], GPIO_OUT);
        gpio_put(LED_PINS[i], 0);
        gpio_init(BTN_PINS[i]);
        gpio_set_dir(BTN_PINS[i], GPIO_IN);
        gpio_pull_down(BTN_PINS[i]);
        gpio_set_irq_enabled_with_callback(BTN_PINS[i], GPIO_IRQ_EDGE_RISE, true, gpio_cb);
    }
    gpio_init(PIN_BTN_PLAY_PAUSE);
    gpio_set_dir(PIN_BTN_PLAY_PAUSE, GPIO_IN);
    gpio_pull_down(PIN_BTN_PLAY_PAUSE);
    gpio_set_irq_enabled_with_callback(PIN_BTN_PLAY_PAUSE, GPIO_IRQ_EDGE_RISE, true, gpio_cb);
}

// ── Main ─────────────────────────────────────────────────────────────────────
int main(void) {
    int sequence[2][MAX_SEQ];
    uint64_t timeout_start = 0;
    setup();
    sequence_init(sequence);
    lcd_menu(menu_sel);

    while (true) {
        switch (state) {

        // ── Menu de seleção de modo ──────────────────────────────────────────
        case ST_MENU:
            if (btn_color != -1) {
                int c = btn_color;
                btn_color = -1;
                if (c == 0) menu_sel = 1;       // botão verde = 1 jogador
                if (c == 1) menu_sel = 2;       // botão vermelho = 2 jogadores
                lcd_menu(menu_sel);
            }
            if (btn_play) {
                btn_play = false;
                num_players = menu_sel;
                state = ST_IDLE;
            }
            break;

        // ── Aguarda PLAY para iniciar ────────────────────────────────────────
        case ST_IDLE: {
            lcd_clear();
            lcd_centered(SCREEN_H / 2 - 30, "GENIUS",         ILI9341_GREEN, 4);
            char sub[24];
            snprintf(sub, sizeof(sub), "%d Jogador%s - PLAY",
                     num_players, num_players == 2 ? "es" : "");
            lcd_centered(SCREEN_H / 2 + 20, sub, ILI9341_WHITE, 2);

            btn_play = false;
            btn_color = -1;
            while (!btn_play) tight_loop_contents();
            btn_play = false;

            cur = 0;
            phase[0] = phase[1] = 1;
            input_idx[0] = input_idx[1] = 0;
            score[0] = score[1] = 0;
            audio_play(PLAY_DATA, PLAY_DATA_LENGTH);
            audio_wait();
            state = ST_SHOW;
            break;
        }

        case ST_SHOW:
            sleep_ms(600);
            for (int i = 0; i < phase[cur]; i++) show_color(sequence[cur][i]);
            input_idx[cur] = 0;
            btn_color = -1;
            btn_play  = false;
            lcd_waiting(cur + 1, score[cur], phase[cur]);
            timeout_reset(&timeout_start);
            state = ST_INPUT;
            break;

        case ST_INPUT:
            if (timeout_expired(timeout_start)) {
                state = ST_LOSE;
                break;
            }
            if (btn_color != -1) {
                int pressed = btn_color;
                btn_color = -1;
                led_on(pressed);
                audio_play(COLOR_DATA[pressed], COLOR_LEN[pressed]);
                audio_wait();
                led_off(pressed);

                if (pressed == sequence[cur][input_idx[cur]]) {
                    input_idx[cur]++;
                    if (input_idx[cur] == phase[cur]) {
                        score[cur]++;
                        if (phase[cur] == MAX_SEQ) {
                            if (num_players == 1 || cur == 1) {
                                state = ST_WIN;
                            } else {
                                cur = 1;
                                state = ST_SHOW;
                            }
                        } else {
                            phase[cur]++;
                            cur = (num_players == 2) ? 1 - cur : 0;
                            state = ST_SHOW;
                        }
                    } else {
                        timeout_reset(&timeout_start);
                    }
                } else {
                    state = ST_LOSE;
                }
            }
            break;

        case ST_WIN:
            all_blink(4);
            lcd_win(score[0], score[1], num_players);
            audio_play(GANHOU_DATA, GANHOU_DATA_LENGTH);
            audio_wait();
            state = ST_MENU;
            menu_sel = num_players;
            lcd_menu(menu_sel);
            break;

        case ST_LOSE:
            all_blink(4);
            lcd_final(score[0], score[1], num_players);
            audio_play(PERDEU_DATA, PERDEU_DATA_LENGTH);
            audio_wait();
            state = ST_MENU;
            menu_sel = num_players;
            lcd_menu(menu_sel);
            break;
        }
    }
}
