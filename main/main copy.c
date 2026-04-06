#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "pinos.h"
#include "tom_verde.h"
#include "tom_vermelho.h"
#include "tom_azul.h"
#include "tom_amarelo.h"
#include "ganhou.h"
#include "perdeu.h"

// ── Sequência ────────────────────────────────────────────────────────────────
#define MAX_SEQ 5
// Para sequência aleatória: substituir por geração com srand/rand
static const int SEQUENCE[MAX_SEQ] = {0, 1, 2, 3, 0};

// ── Áudio stub (substituir pelo driver PWM/DMA real) ─────────────────────────
static inline void audio_play(const uint8_t *data, uint32_t len) {
    (void)data; (void)len;
}

static const uint8_t  *COLOR_DATA[NUM_COLORS] = {VERDE_DATA, VERMELHO_DATA, AZUL_DATA, AMARELO_DATA};
static const uint32_t  COLOR_LEN[NUM_COLORS]  = {VERDE_DATA_LENGTH, VERMELHO_DATA_LENGTH, AZUL_DATA_LENGTH, AMARELO_DATA_LENGTH};

// ── LCD stub (substituir pelo driver ILI9341 real) ───────────────────────────
static inline void lcd_show_score(int s)          { (void)s; }
static inline void lcd_show_state(const char *m)  { (void)m; }

// ── Estados ──────────────────────────────────────────────────────────────────
typedef enum { ST_IDLE, ST_SHOW, ST_INPUT, ST_PAUSED, ST_WIN, ST_LOSE } State;

static volatile int  btn_color = -1;
static volatile bool btn_play  = false;
static State state     = ST_IDLE;
static int   phase     = 1;
static int   input_idx = 0;
static int   score     = 0;

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

static void play_color(int c) {
    led_on(c);
    audio_play(COLOR_DATA[c], COLOR_LEN[c]);
    sleep_ms(500);
    led_off(c);
    sleep_ms(80);
}

// ── ISR ──────────────────────────────────────────────────────────────────────
static void gpio_cb(uint gpio, uint32_t events) {
    if (!(events & GPIO_IRQ_EDGE_RISE)) 
        return;
    if (gpio == PIN_BTN_PLAY_PAUSE) { 
        btn_play = true; 
        return; 
    }
    for (int i = 0; i < NUM_COLORS; i++)
        if (gpio == BTN_PINS[i]) { 
            btn_color = i; 
            return; 
        }
}

// ── Setup ────────────────────────────────────────────────────────────────────
static void setup(void) {
    stdio_init_all();
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
    setup();

    while (true) {
        if (btn_play) {
            btn_play = false;
            if (state == ST_IDLE || state == ST_WIN || state == ST_LOSE) {
                phase = 1; 
                input_idx = 0; 
                score = 0;
                audio_play(WAV_DATA, WAV_DATA_LENGTH);
                lcd_show_state("PLAYING");
                state = ST_SHOW;
            } else if (state == ST_PAUSED) {
                audio_play(WAV_DATA, WAV_DATA_LENGTH);
                lcd_show_state("PLAYING");
                state = ST_SHOW;
            } else {
                audio_play(WAV_DATA, WAV_DATA_LENGTH);
                lcd_show_state("PAUSED");
                state = ST_PAUSED;
            }
        }

        switch (state) {
        case ST_IDLE:
        case ST_PAUSED:
            break;

        case ST_SHOW:
            sleep_ms(600);
            for (int i = 0; i < phase; i++) play_color(SEQUENCE[i]);
            input_idx = 0;
            btn_color = -1;
            state = ST_INPUT;
            break;

        case ST_INPUT:
            if (btn_color != -1) {
                int pressed = btn_color;
                btn_color = -1;
                play_color(pressed);
                if (pressed == SEQUENCE[input_idx]) {
                    input_idx++;
                    if (input_idx == phase) {
                        score++;
                        lcd_show_score(score);
                        state = (phase == MAX_SEQ) ? ST_WIN : ST_SHOW;
                        if (state == ST_SHOW) phase++;
                    }
                } else {
                    state = ST_LOSE;
                }
            }
            break;

        case ST_WIN:
            all_blink(4);
            audio_play(GANHOU_DATA, GANHOU_DATA_LENGTH);
            lcd_show_state("YOU WIN!");
            lcd_show_score(score);
            state = ST_IDLE;
            break;

        case ST_LOSE:
            all_blink(4);
            audio_play(PERDEU_DATA, PERDEU_DATA_LENGTH);
            lcd_show_state("GAME OVER");
            lcd_show_score(score);
            state = ST_IDLE;
            break;
        }
    }
}
