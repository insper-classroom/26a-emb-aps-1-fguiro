#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"

// #include "ganhou.h"
// #include "perdeu.h"
// #include "tom_amarelo.h"
// #include "tom_azul.h"
// #include "tom_verde.h"
// #include "tom_vermelho.h"

#include "pinos.h"

// ─── Sequência fixa ───────────────────────────────────────────────────────────
#define MAX_SEQ 5
const int sequence[MAX_SEQ] = {0, 1, 2, 3, 0};

// ─── Estado global ────────────────────────────────────────────────────────────
typedef enum { 
    SHOW_SEQUENCE, 
    WAIT_INPUT, 
    GAME_OVER
} GameState;

volatile int btn_flag    = -1;
GameState    state       = SHOW_SEQUENCE;
int          phase       = 1;
int          input_index = 0;

// ─── Helpers de LED ───────────────────────────────────────────────────────────
void led_on(int color)  { gpio_put(LED_PINS[color], 1); }
void led_off(int color) { gpio_put(LED_PINS[color], 0); }
void all_leds_off(void) { for (int i = 0; i < NUM_COLORS; i++) led_off(i); }

void all_leds_blink(int times) {
    for (int i = 0; i < times; i++) {
        for (int c = 0; c < NUM_COLORS; c++) led_on(c);
        sleep_ms(300);
        all_leds_off();
        sleep_ms(300);
    }
}

// ─── Funções de cor, ganhou e perdeu ──────────────────────────────────────────
void play_color(int color) {
    led_on(color);
    // audio_play(AUDIO_DATA[color], AUDIO_LEN[color]);
    sleep_ms(500);  // simula duração do som
    led_off(color);
    sleep_ms(80);
}

void play_lose(void) {
    all_leds_blink(4);
    // audio_play(PERDEU_DATA, PERDEU_LENGTH);
}

void play_win(void) {
    all_leds_blink(4);
    // audio_play(GANHOU_DATA, GANHOU_LENGTH);
}

// ─── ISR dos botões ───────────────────────────────────────────────────────────
void gpio_callback(uint gpio, uint32_t events) {
    if (events & GPIO_IRQ_EDGE_RISE) {
        for (int i = 0; i < NUM_COLORS; i++) {
            if (gpio == BTN_PINS[i]) {
                btn_flag = i;
                break;
            }
        }
    }
}

// ─── Inicialização ────────────────────────────────────────────────────────────
void setup(void) {
    stdio_init_all();

    for (int i = 0; i < NUM_COLORS; i++) {
        gpio_init(LED_PINS[i]);
        gpio_set_dir(LED_PINS[i], GPIO_OUT);
        gpio_put(LED_PINS[i], 0);

        gpio_init(BTN_PINS[i]);
        gpio_set_dir(BTN_PINS[i], GPIO_IN);
        gpio_pull_down(BTN_PINS[i]);

        gpio_set_irq_enabled_with_callback(
            BTN_PINS[i],
            GPIO_IRQ_EDGE_RISE,
            true,
            &gpio_callback
        );
    }
}

// ─── Main ─────────────────────────────────────────────────────────────────────
int main(void) {
    setup();
    // set_sys_clock_khz(176000, true);
    // pwm_audio_init();

    while (true) {
        if (state == SHOW_SEQUENCE) {
            sleep_ms(600);
            for (int i = 0; i < phase; i++) {
                play_color(sequence[i]);
            }
            input_index = 0;
            btn_flag    = -1;
            state       = WAIT_INPUT;

        } else if (state == WAIT_INPUT) {
            if (btn_flag != -1) {
                int pressed = btn_flag;
                btn_flag    = -1;

                play_color(pressed);

                if (pressed == sequence[input_index]) {
                    input_index++;
                    if (input_index == phase) {
                        if (phase == MAX_SEQ) {
                            play_win();
                            phase = 1;
                        } else {
                            phase++;
                        }
                        state = SHOW_SEQUENCE;
                    }
                } else {
                    state = GAME_OVER;
                }
            }

        } else if (state == GAME_OVER) {
            play_lose();
            phase       = 1;
            input_index = 0;
            state       = SHOW_SEQUENCE;
        }
    }
}