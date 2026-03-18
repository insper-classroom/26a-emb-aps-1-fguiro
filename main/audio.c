/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"  // pwm 
#include "hardware/sync.h" // wait for interrupt 
#include "hardware/clocks.h"

#include "eu_sou_felipe.h"
#include "gorilla.h"

const int BTN_BLACK_PIN = 4;
const int BTN_WHITE_PIN = 6;
const int BTN_RED_PIN = 10;

const int AUDIO_PIN = 28;

volatile int audio_gorilla = 0;
volatile int audio_felipe = 0;
volatile int pause = 0;

volatile int black_button = 0;
volatile int white_button = 0;
volatile int red_button = 0;

volatile int wav_position = 0;

volatile uint32_t last_button_time = 0;

void btn_callback(uint gpio, uint32_t events) {
    uint32_t now = time_us_32();
    if (now - last_button_time < 200000) {  // 200ms debounce
        return;
    }
    last_button_time = now;

    if (events == 0x4) {  // fall edge
        if (gpio == BTN_BLACK_PIN) {
            audio_gorilla = 1;
            audio_felipe = 0;
            pause = 0;
            wav_position = 0;
        } else if (gpio == BTN_WHITE_PIN) {
            audio_felipe = 1;
            audio_gorilla = 0;
            pause = 0;
            wav_position = 0;
        } else if (gpio == BTN_RED_PIN) {
            pause = !pause;
        }
    } 
}

void pwm_interrupt_handler() {
    pwm_clear_irq(pwm_gpio_to_slice_num(AUDIO_PIN));  
    if (pause){
        pwm_set_gpio_level(AUDIO_PIN, 0);
        return;
    }
    
    if (audio_felipe) { 
        if (wav_position < (FELIPE_DATA_LENGTH<<3) - 1) { 
            // set pwm level 
            // allow the pwm value to repeat for 8 cycles this is >>3 
            pwm_set_gpio_level(AUDIO_PIN, FELIPE_DATA[wav_position>>3]);  
            wav_position++;
        } else {
            // reset to start
            wav_position = 0;
        }
    } else if (audio_gorilla){
        if (wav_position < (GORILLA_DATA_LENGTH<<3) - 1) { 
            // set pwm level 
            // allow the pwm value to repeat for 8 cycles this is >>3 
            pwm_set_gpio_level(AUDIO_PIN, GORILLA_DATA[wav_position>>3]);  
            wav_position++;
        } else {
            // reset to start
            wav_position = 0;
        }
    } else {
        // Nenhum áudio ativo: silêncio
        pwm_set_gpio_level(AUDIO_PIN, 0);
    }
}

void setup(){
    gpio_init(BTN_BLACK_PIN);
    gpio_set_dir(BTN_BLACK_PIN, GPIO_IN);
    gpio_pull_up(BTN_BLACK_PIN);

    gpio_init(BTN_WHITE_PIN);
    gpio_set_dir(BTN_WHITE_PIN, GPIO_IN);
    gpio_pull_up(BTN_WHITE_PIN);

    gpio_init(BTN_RED_PIN);
    gpio_set_dir(BTN_RED_PIN, GPIO_IN);
    gpio_pull_up(BTN_RED_PIN);  

    gpio_set_irq_enabled_with_callback(BTN_BLACK_PIN, GPIO_IRQ_EDGE_FALL, true, &btn_callback);
    gpio_set_irq_enabled(BTN_WHITE_PIN, GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_enabled(BTN_RED_PIN, GPIO_IRQ_EDGE_FALL, true);

    gpio_set_dir(AUDIO_PIN, GPIO_OUT);
}

int main() {
    stdio_init_all();
    setup();

    set_sys_clock_khz(176000, true); 
    gpio_set_function(AUDIO_PIN, GPIO_FUNC_PWM);

    int audio_pin_slice = pwm_gpio_to_slice_num(AUDIO_PIN);

    // Setup PWM interrupt to fire when PWM cycle is complete
    pwm_clear_irq(audio_pin_slice);
    pwm_set_irq_enabled(audio_pin_slice, true);
    // set the handle function above
    irq_set_exclusive_handler(PWM_IRQ_WRAP, pwm_interrupt_handler); 
    irq_set_enabled(PWM_IRQ_WRAP, true);

    // Setup PWM for audio output
    pwm_config config = pwm_get_default_config();
    /* Base clock 176,000,000 Hz divide by wrap 250 then the clock divider further divides
     * to set the interrupt rate. 
     * 
     * 11 KHz is fine for speech. Phone lines generally sample at 8 KHz
     * 
     * 
     * So clkdiv should be as follows for given sample rate
     *  8.0f for 11 KHz
     *  4.0f for 22 KHz
     *  2.0f for 44 KHz etc
     */
    pwm_config_set_clkdiv(&config, 8.0f); 
    pwm_config_set_wrap(&config, 250); 
    pwm_init(audio_pin_slice, &config, true);

    pwm_set_gpio_level(AUDIO_PIN, 0);

    while (true) {
        __wfi();  // Wait for interrupt
    }
}
