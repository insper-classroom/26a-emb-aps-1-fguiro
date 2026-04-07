#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "hardware/pwm.h"

#include "pinos.h"
#include "tom_verde.h"
#include "tom_vermelho.h"
#include "tom_amarelo.h"
#include "tom_azul.h"
#include "ganhou.h"
#include "perdeu.h"
#include "play.h"

// ── Sequência ────────────────────────────────────────────────────────────────
#define MAX_SEQ 5

static void sequence_init(int seq[2][MAX_SEQ]) {
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < MAX_SEQ; i++)
            seq[p][i] = rand() % NUM_COLORS;
}

// ── Timeout ──────────────────────────────────────────────────────────────────
#define INPUT_TIMEOUT_US (7 * 1000000ULL)
static void timeout_reset(uint64_t *start) { *start = time_us_64(); }
static bool timeout_expired(uint64_t start) { return (time_us_64() - start) >= INPUT_TIMEOUT_US; }

// ── Áudio (PWM + repeating timer, não-bloqueante) ─────────────────────────────
// Sample rate dos arquivos: 11000 Hz → período ≈ 90 µs
#define AUDIO_TIMER_US (1000000 / 11000)

static struct {
    const uint8_t        *data;
    uint32_t              length;
    uint32_t              pos;
    volatile bool         playing;
    uint                  slice;
    uint                  channel;
    struct repeating_timer timer;
} g_audio;

static bool audio_timer_cb(struct repeating_timer *t) {
    (void)t;
    if (!g_audio.playing)
        return false;
    if (g_audio.pos >= g_audio.length) {
        g_audio.playing = false;
        pwm_set_chan_level(g_audio.slice, g_audio.channel, 0);
        return false;
    }
    pwm_set_chan_level(g_audio.slice, g_audio.channel, g_audio.data[g_audio.pos]);
    g_audio.pos++;
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

    g_audio.playing = false;
}

static void audio_play(const uint8_t *data, uint32_t length) {
    cancel_repeating_timer(&g_audio.timer);
    g_audio.data    = data;
    g_audio.length  = length;
    g_audio.pos     = 0;
    g_audio.playing = true;
    add_repeating_timer_us(-AUDIO_TIMER_US, audio_timer_cb, NULL, &g_audio.timer);
}

static void audio_stop(void) {
    g_audio.playing = false;
    cancel_repeating_timer(&g_audio.timer);
    pwm_set_chan_level(g_audio.slice, g_audio.channel, 0);
}

static void audio_wait(void) {
    while (g_audio.playing) tight_loop_contents();
}

// ── LEDs ─────────────────────────────────────────────────────────────────────
static void led_on(int c)  { gpio_put(LED_PINS[c], 1); }
static void led_off(int c) { gpio_put(LED_PINS[c], 0); }

static void all_off(void) {
    for (int i = 0; i < NUM_COLORS; i++) led_off(i);
}

static void all_blink(int n) {
    for (int i = 0; i < n; i++) {
        for (int c = 0; c < NUM_COLORS; c++) led_on(c);
        sleep_ms(300);
        all_off();
        sleep_ms(300);
    }
}

// Tabela de áudio por cor (0:verde 1:vermelho 2:amarelo 3:azul)
static uint8_t *const COLOR_AUDIO[]  = {VERDE_DATA,        VERMELHO_DATA,        AMARELO_DATA,        AZUL_DATA};
static const uint32_t COLOR_LENGTH[] = {VERDE_DATA_LENGTH, VERMELHO_DATA_LENGTH, AMARELO_DATA_LENGTH, AZUL_DATA_LENGTH};

static void show_color(int c) {
    audio_play(COLOR_AUDIO[c], COLOR_LENGTH[c]);
    led_on(c);
    sleep_ms(500);
    led_off(c);
    audio_stop();
    sleep_ms(150);
}

// ── Polling de botões com debounce ───────────────────────────────────────────
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

// ── Tipos de estado ───────────────────────────────────────────────────────────
typedef enum { ST_MENU, ST_SHOW, ST_INPUT, ST_WIN, ST_LOSE } State;

// ── Main ─────────────────────────────────────────────────────────────────────
int main(void) {
    // Variáveis de jogo (locais ao main — sem globais desnecessárias)
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

    while (true) {
        switch (state) {

        // ── Menu: verde = 1P, vermelho = 2P, PLAY = confirma e inicia ─────────
        case ST_MENU: {
            int c = poll_color_btn();
            if (c == 0) {
                menu_sel = 1;
                led_on(0); sleep_ms(200); led_off(0);
                printf("Menu: 1 jogador\n");
            }
            if (c == 1) {
                menu_sel = 2;
                led_on(1); sleep_ms(200); led_off(1);
                printf("Menu: 2 jogadores\n");
            }
            if (poll_play_btn()) {
                num_players  = menu_sel;
                cur          = 0;
                phase[0]     = 1;
                phase[1]     = 1;
                input_idx[0] = 0;
                input_idx[1] = 0;
                score[0]     = 0;
                score[1]     = 0;
                sequence_init(sequence);
                audio_play(PLAY_DATA, PLAY_DATA_LENGTH);
                printf("Jogo iniciado: %d jogador(es)\n", num_players);
                state = ST_SHOW;
            }
            break;
        }

        // ── Exibe a sequência via LEDs ────────────────────────────────────────
        case ST_SHOW:
            sleep_ms(600);
            printf("Mostrando sequencia P%d fase %d: ", cur + 1, phase[cur]);
            for (int i = 0; i < phase[cur]; i++) {
                printf("%d ", sequence[cur][i]);
                show_color(sequence[cur][i]);
            }
            printf("\n");
            input_idx[cur] = 0;
            timeout_reset(&timeout_start);
            state = ST_INPUT;
            break;

        // ── Lê a entrada do jogador ───────────────────────────────────────────
        case ST_INPUT: {
            if (timeout_expired(timeout_start)) {
                printf("Timeout!\n");
                state = ST_LOSE;
                break;
            }
            int pressed = poll_color_btn();
            if (pressed == -1) break;

            printf("Botao: %d (esperado: %d)\n", pressed, sequence[cur][input_idx[cur]]);
            audio_play(COLOR_AUDIO[pressed], COLOR_LENGTH[pressed]);
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
                // Ainda faltam cores nesta fase
                timeout_reset(&timeout_start);
                break;
            }

            // Fase completa
            score[cur]++;
            printf("P%d completou fase %d! Score: %d\n", cur + 1, phase[cur], score[cur]);

            if (phase[cur] == MAX_SEQ) {
                // Último nível: em 2P P1 passa a vez; se já é P2 (cur==1), vitória
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
            audio_play(GANHOU_DATA, GANHOU_DATA_LENGTH);
            all_blink(4);
            audio_stop();
            state    = ST_MENU;
            menu_sel = num_players;
            break;

        // ── Derrota: pisca a cor correta 3x e toca perdeu.h ──────────────────
        case ST_LOSE:
            printf("Perdeu! P1:%d P2:%d\n", score[0], score[1]);
            audio_play(PERDEU_DATA, PERDEU_DATA_LENGTH);
            for (int i = 0; i < 3; i++) {
                led_on(sequence[cur][input_idx[cur]]);
                sleep_ms(150);
                led_off(sequence[cur][input_idx[cur]]);
                sleep_ms(150);
            }
            audio_wait();
            state    = ST_MENU;
            menu_sel = num_players;
            break;
        }
    }
}
