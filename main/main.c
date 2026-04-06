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

static int sequence[MAX_SEQ];
static void sequence_init(void) {
    static const int fixed[MAX_SEQ] = {0, 1, 2, 3, 0};
    for (int i = 0; i < MAX_SEQ; i++) sequence[i] = fixed[i];
}

// ── Áudio PWM ────────────────────────────────────────────────────────────────
#define AUDIO_SAMPLE_RATE 11000
#define PWM_WRAP          255

static const uint8_t  *COLOR_DATA[NUM_COLORS] = {VERDE_DATA, VERMELHO_DATA, AZUL_DATA, AMARELO_DATA};
static const uint32_t  COLOR_LEN[NUM_COLORS]  = {VERDE_DATA_LENGTH, VERMELHO_DATA_LENGTH, AZUL_DATA_LENGTH, AMARELO_DATA_LENGTH};

static const uint8_t    *audio_buf = NULL;
static uint32_t          audio_len = 0;
static volatile uint32_t audio_pos = 0;
static volatile bool     audio_done = true;

static bool audio_timer_cb(struct repeating_timer *t) {
    (void)t;
    if (audio_done) return true;
    if (audio_pos < audio_len)
        pwm_set_gpio_level(BUZZER_PIN, audio_buf[audio_pos++]);
    else {
        pwm_set_gpio_level(BUZZER_PIN, 128);
        audio_done = true;
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
    audio_done = true;
    audio_buf  = data;
    audio_len  = len;
    audio_pos  = 0;
    audio_done = false;
}

static void audio_wait(void) {
    while (!audio_done) tight_loop_contents();
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

static void lcd_idle(void) {
    lcd_clear();
    lcd_centered(SCREEN_H / 2 - 30, "GENIUS",         ILI9341_GREEN, 4);
    lcd_centered(SCREEN_H / 2 + 20, "Pressione PLAY", ILI9341_WHITE, 2);
}

static void lcd_waiting(int score, int phase) {
    lcd_clear();
    lcd_centered(SCREEN_H / 2 - 20, "Sua vez!", ILI9341_CYAN, 3);
    gfx_setTextSize(2);
    gfx_setTextColor(ILI9341_WHITE);
    char buf[32];
    snprintf(buf, sizeof(buf), "Fase:%d  Pts:%d", phase, score);
    gfx_drawText(8, SCREEN_H - 24, buf);
}

static void lcd_result(const char *msg, uint16_t color, int score) {
    lcd_clear();
    lcd_centered(SCREEN_H / 2 - 30, msg, color, 3);
    char buf[24];
    snprintf(buf, sizeof(buf), "Pontos: %d", score);
    lcd_centered(SCREEN_H / 2 + 20, buf, ILI9341_WHITE, 2);
}

// ── Timeout ──────────────────────────────────────────────────────────────────
#define INPUT_TIMEOUT_US (7 * 1000000ULL)
static uint64_t timeout_start = 0;
static void timeout_reset(void) { timeout_start = time_us_64(); }
static bool timeout_expired(void) { return (time_us_64() - timeout_start) >= INPUT_TIMEOUT_US; }

// ── Estados ──────────────────────────────────────────────────────────────────
typedef enum { ST_IDLE, ST_SHOW, ST_INPUT, ST_WIN, ST_LOSE } State;

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
    if (gpio == PIN_BTN_PLAY_PAUSE) { btn_play = true; return; }
    for (int i = 0; i < NUM_COLORS; i++)
        if (gpio == BTN_PINS[i]) { btn_color = i; return; }
}

// ── Setup ────────────────────────────────────────────────────────────────────
static void setup(void) {
    stdio_init_all();
    LCD_initDisplay();
    LCD_setRotation(SCREEN_ROTATION);
    gfx_init();
    gfx_clear();
    audio_init();
    sequence_init();

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
    lcd_idle();

    while (true) {
        switch (state) {
        case ST_IDLE:
            if (btn_play) {
                btn_play = false;
                phase = 1; input_idx = 0; score = 0;
                audio_play(PLAY_DATA, PLAY_DATA_LENGTH);
                audio_wait();
                state = ST_SHOW;
            }
            break;

        case ST_SHOW:
            sleep_ms(600);
            for (int i = 0; i < phase; i++) show_color(sequence[i]);
            input_idx = 0;
            btn_color = -1;
            btn_play  = false;
            lcd_waiting(score, phase);
            timeout_reset();
            state = ST_INPUT;
            break;

        case ST_INPUT:
            if (timeout_expired()) {
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

                if (pressed == sequence[input_idx]) {
                    input_idx++;
                    if (input_idx == phase) {
                        score++;
                        if (phase == MAX_SEQ) {
                            state = ST_WIN;
                        } else {
                            phase++;
                            state = ST_SHOW;
                        }
                    } else {
                        timeout_reset();
                    }
                } else {
                    state = ST_LOSE;
                }
            }
            break;

        case ST_WIN:
            all_blink(4);
            lcd_result("YOU WIN!", ILI9341_GREEN, score);
            audio_play(GANHOU_DATA, GANHOU_DATA_LENGTH);
            audio_wait();
            state = ST_IDLE;
            lcd_idle();
            break;

        case ST_LOSE:
            all_blink(4);
            lcd_result("GAME OVER", ILI9341_RED, score);
            audio_play(PERDEU_DATA, PERDEU_DATA_LENGTH);
            audio_wait();
            state = ST_IDLE;
            lcd_idle();
            break;
        }
    }
}
