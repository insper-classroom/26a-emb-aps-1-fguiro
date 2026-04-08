#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "tom_amarelo.h"
#include "tom_azul.h"
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
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < MAX_SEQ; i++)
            seq[p][i] = rand() % NUM_COLORS;
}

// ── Áudio PWM (carrier ~488 kHz, amostras a 11 kHz via timer) ────────────────
// Usado pela IRQ do timer — precisa ser global
typedef struct {
    const uint8_t    *buf;
    uint32_t          len;
    volatile uint32_t pos;
    volatile bool     done;
    uint              slice;
    uint              channel;
    struct repeating_timer timer;
} AudioState;
static AudioState g_audio = {NULL, 0, 0, true, 0, 0};

static bool audio_timer_cb(struct repeating_timer *t) {
    (void)t;
    if (g_audio.done) return true;
    if (g_audio.pos < g_audio.len)
        pwm_set_chan_level(g_audio.slice, g_audio.channel, g_audio.buf[g_audio.pos++]);
    else {
        pwm_set_chan_level(g_audio.slice, g_audio.channel, 0);
        g_audio.done = true;
    }
    return true;
}

static void audio_init(void) {
    gpio_set_function(BUZZER_PIN, GPIO_FUNC_PWM);
    g_audio.slice   = pwm_gpio_to_slice_num(BUZZER_PIN);
    g_audio.channel = pwm_gpio_to_channel(BUZZER_PIN);

    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&cfg, 1.0f);  // carrier ~488 kHz (inaudível)
    pwm_config_set_wrap(&cfg, 255);     // resolução 8-bit
    pwm_init(g_audio.slice, &cfg, true);
    pwm_set_chan_level(g_audio.slice, g_audio.channel, 0);

    add_repeating_timer_us(-(1000000 / 11000), audio_timer_cb, NULL, &g_audio.timer);
}

static void audio_play(const uint8_t *data, uint32_t len) {
    g_audio.done = true;
    g_audio.buf  = data;
    g_audio.len  = len;
    g_audio.pos  = 0;
    g_audio.done = false;
}

static void audio_stop(void) {
    g_audio.done = true;
    pwm_set_chan_level(g_audio.slice, g_audio.channel, 0);
}

static void audio_wait(void) {
    while (!g_audio.done) tight_loop_contents();
}

// ── Tabela de áudio por cor (0:verde 1:vermelho 2:amarelo 3:azul) ─────────────
static const uint8_t *const COLOR_DATA[]  = {VERDE_DATA, VERMELHO_DATA, AMARELO_DATA, AZUL_DATA};
static const uint32_t       COLOR_LEN[]   = {VERDE_DATA_LENGTH, VERMELHO_DATA_LENGTH,
                                              AMARELO_DATA_LENGTH, AZUL_DATA_LENGTH};

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

    uint16_t c1 = (selected == 1) ? ILI9341_GREEN  : ILI9341_WHITE;
    uint16_t c2 = (selected == 2) ? ILI9341_YELLOW : ILI9341_WHITE;
    lcd_centered(SCREEN_H / 2 - 10, "[Verde]  1 Jogador",   c1, 2);
    lcd_centered(SCREEN_H / 2 + 25, "[Verm.]  2 Jogadores", c2, 2);
    lcd_centered(SCREEN_H - 24, "PLAY para confirmar", ILI9341_WHITE, 1);
}

static void lcd_sua_vez(int player, int score, int phase) {
    lcd_clear();
    char who[16];
    if (player > 0) {
        snprintf(who, sizeof(who), "Vez do P%d!", player);
        lcd_centered(SCREEN_H / 2 - 40, who,
                     player == 1 ? ILI9341_CYAN : ILI9341_YELLOW, 2);
    }
    lcd_centered(SCREEN_H / 2 - 5, "SUA VEZ!", ILI9341_WHITE, 3);
    char buf[32];
    snprintf(buf, sizeof(buf), "Fase:%d  Pts:%d", phase, score);
    gfx_setTextSize(2);
    gfx_setTextColor(ILI9341_WHITE);
    gfx_drawText(8, SCREEN_H - 24, buf);
}

static void lcd_final(bool win, int s1, int s2, int nplayers) {
    lcd_clear();
    if (win) {
        lcd_centered(SCREEN_H / 2 - 50, "YOU WIN!", ILI9341_GREEN, 3);
    } else {
        lcd_centered(SCREEN_H / 2 - 50, "GAME OVER!", ILI9341_RED, 3);
    }
    char buf[32];
    if (nplayers == 2) {
        snprintf(buf, sizeof(buf), "P1: %d pts", s1);
        lcd_centered(SCREEN_H / 2 + 0,  buf, ILI9341_CYAN,   2);
        snprintf(buf, sizeof(buf), "P2: %d pts", s2);
        lcd_centered(SCREEN_H / 2 + 30, buf, ILI9341_YELLOW, 2);
    } else {
        snprintf(buf, sizeof(buf), "Pontos: %d/%d", s1, MAX_SEQ);
        lcd_centered(SCREEN_H / 2 + 10, buf, ILI9341_WHITE, 2);
    }
}

// ── Timeout ──────────────────────────────────────────────────────────────────
#define INPUT_TIMEOUT_US (7 * 1000000ULL)
static void timeout_reset(uint64_t *start) { *start = time_us_64(); }
static bool timeout_expired(uint64_t start) { return (time_us_64() - start) >= INPUT_TIMEOUT_US; }

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
    audio_play(COLOR_DATA[c], COLOR_LEN[c]);
    led_on(c);
    sleep_ms(500);
    led_off(c);
    audio_stop();
    sleep_ms(150);
}

// ── Polling de botões (ativo em LOW, pull-up) ─────────────────────────────────
static int poll_color_btn(void) {
    for (int i = 0; i < NUM_COLORS; i++) {
        if (!gpio_get(BTN_PINS[i])) {
            sleep_ms(20);
            if (!gpio_get(BTN_PINS[i])) {
                while (!gpio_get(BTN_PINS[i])) tight_loop_contents();
                return i;
            }
        }
    }
    return -1;
}

static bool poll_play_btn(void) {
    if (!gpio_get(PIN_BTN_PLAY_PAUSE)) {
        sleep_ms(20);
        if (!gpio_get(PIN_BTN_PLAY_PAUSE)) {
            while (!gpio_get(PIN_BTN_PLAY_PAUSE)) tight_loop_contents();
            return true;
        }
    }
    return false;
}

// ── Setup ────────────────────────────────────────────────────────────────────
static void setup(void) {
    stdio_init_all();
    sleep_ms(2000);
    srand((unsigned int)time_us_64());

    LCD_initDisplay();
    LCD_setRotation(SCREEN_ROTATION);
    gfx_init();
    lcd_clear();

    for (int i = 0; i < NUM_COLORS; i++) {
        gpio_init(LED_PINS[i]);
        gpio_set_dir(LED_PINS[i], GPIO_OUT);
        gpio_put(LED_PINS[i], 0);

        gpio_init(BTN_PINS[i]);
        gpio_set_dir(BTN_PINS[i], GPIO_IN);
        gpio_pull_up(BTN_PINS[i]);
    }

    gpio_init(PIN_BTN_PLAY_PAUSE);
    gpio_set_dir(PIN_BTN_PLAY_PAUSE, GPIO_IN);
    gpio_pull_up(PIN_BTN_PLAY_PAUSE);

    audio_init();
    all_blink(2);
    printf("Setup OK\n");
}

// ── Tipo de estado ────────────────────────────────────────────────────────────
typedef enum { ST_MENU, ST_SHOW, ST_INPUT, ST_WIN, ST_LOSE } State;

// ── Main ─────────────────────────────────────────────────────────────────────
int main(void) {
    int sequence[2][MAX_SEQ];
    uint64_t timeout_start = 0;
    State state     = ST_MENU;
    int num_players = 1;
    int menu_sel    = 1;
    int phase[2]     = {1, 1};
    int input_idx[2] = {0, 0};
    int score[2]     = {0, 0};
    int cur = 0;

    setup();
    lcd_menu(menu_sel);

    while (true) {
        switch (state) {

        // ── Menu: verde=1P, vermelho=2P, PLAY=iniciar ─────────────────────────
        case ST_MENU: {
            int c = poll_color_btn();
            if (c == 0) { menu_sel = 1; lcd_menu(menu_sel); }
            if (c == 1) { menu_sel = 2; lcd_menu(menu_sel); }
            if (poll_play_btn()) {
                num_players  = menu_sel;
                cur          = 0;
                phase[0]     = 1;  phase[1]     = 1;
                input_idx[0] = 0;  input_idx[1] = 0;
                score[0]     = 0;  score[1]     = 0;
                sequence_init(sequence);
                audio_play(PLAY_DATA, PLAY_DATA_LENGTH);
                audio_wait();
                state = ST_SHOW;
            }
            break;
        }

        // ── Mostra sequência ──────────────────────────────────────────────────
        case ST_SHOW:
            sleep_ms(600);
            printf("Sequencia P%d fase %d: ", cur + 1, phase[cur]);
            for (int i = 0; i < phase[cur]; i++) {
                printf("%d ", sequence[cur][i]);
                show_color(sequence[cur][i]);
            }
            printf("\n");
            input_idx[cur] = 0;
            lcd_sua_vez(num_players == 2 ? cur + 1 : 0, score[cur], phase[cur]);
            timeout_reset(&timeout_start);
            state = ST_INPUT;
            break;

        // ── Entrada do jogador ────────────────────────────────────────────────
        case ST_INPUT: {
            if (timeout_expired(timeout_start)) {
                printf("Timeout!\n");
                state = ST_LOSE;
                break;
            }
            int pressed = poll_color_btn();
            if (pressed == -1) break;

            printf("Botao: %d (esperado: %d)\n", pressed, sequence[cur][input_idx[cur]]);
            audio_play(COLOR_DATA[pressed], COLOR_LEN[pressed]);
            led_on(pressed);
            sleep_ms(200);
            led_off(pressed);
            audio_stop();

            if (pressed != sequence[cur][input_idx[cur]]) {
                state = ST_LOSE;
                break;
            }

            input_idx[cur]++;
            if (input_idx[cur] < phase[cur]) {
                timeout_reset(&timeout_start);
                break;
            }

            // Fase completa
            score[cur]++;
            printf("P%d completou fase %d! Score: %d\n", cur + 1, phase[cur], score[cur]);

            if (phase[cur] == MAX_SEQ) {
                if (num_players == 2 && cur == 0) {
                    cur   = 1;
                    state = ST_SHOW;
                } else {
                    state = ST_WIN;
                }
            } else {
                phase[cur]++;
                cur   = (num_players == 2) ? 1 - cur : 0;
                state = ST_SHOW;
            }
            break;
        }

        // ── Vitória ───────────────────────────────────────────────────────────
        case ST_WIN:
            printf("Ganhou! P1:%d P2:%d\n", score[0], score[1]);
            lcd_final(true, score[0], score[1], num_players);
            audio_play(GANHOU_DATA, GANHOU_DATA_LENGTH);
            all_blink(4);
            audio_stop();
            sleep_ms(3000);
            state    = ST_MENU;
            menu_sel = num_players;
            lcd_menu(menu_sel);
            break;

        // ── Derrota ───────────────────────────────────────────────────────────
        case ST_LOSE:
            printf("Perdeu! P1:%d P2:%d\n", score[0], score[1]);
            lcd_final(false, score[0], score[1], num_players);
            audio_play(PERDEU_DATA, PERDEU_DATA_LENGTH);
            for (int i = 0; i < 3; i++) {
                led_on(sequence[cur][input_idx[cur]]);
                sleep_ms(150);
                led_off(sequence[cur][input_idx[cur]]);
                sleep_ms(150);
            }
            audio_wait();
            sleep_ms(2000);
            state    = ST_MENU;
            menu_sel = num_players;
            lcd_menu(menu_sel);
            break;
        }
    }
}