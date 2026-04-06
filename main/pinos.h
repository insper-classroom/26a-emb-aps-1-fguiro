#ifndef PINOS_H
#define PINOS_H

#define NUM_COLORS 4

// LEDs
#define PIN_LED_VERDE    7
#define PIN_LED_VERMELHO 5
#define PIN_LED_AMARELO  4
#define PIN_LED_AZUL     6

// Botões
#define PIN_BTN_VERDE    22
#define PIN_BTN_VERMELHO 27
#define PIN_BTN_AMARELO  28
#define PIN_BTN_AZUL     26

#define PIN_BTN_PLAY_PAUSE 10

// Buzzer
#define BUZZER_PIN  11

// Arrays (definidos aqui como static para evitar múltipla definição)
static const uint LED_PINS[NUM_COLORS] = {PIN_LED_VERDE, PIN_LED_VERMELHO, PIN_LED_AMARELO, PIN_LED_AZUL};
static const uint BTN_PINS[NUM_COLORS] = {PIN_BTN_VERDE, PIN_BTN_VERMELHO, PIN_BTN_AMARELO, PIN_BTN_AZUL};

// Frequências do buzzer por cor (Hz)
static const uint FREQS[NUM_COLORS] = {392, 330, 294, 247};

#endif // PINOS_H