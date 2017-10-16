#include <Arduino.h>
#include <ArduinoJson.h>
#include "ESPixelStick.h"
#include "pwm.h"

extern  config_t    config;         // Current configuration

// PWM globals

uint16_t last_pwm[NUM_GPIO];   // 0-255, 0=dark


#if defined(ESPS_SUPPORT_PWM)
void setupPWM () {
  if ( config.pwm_enabled ) {
    if ( (config.pwm_freq >= 100) && (config.pwm_freq <= 1000) ) {
      analogWriteFreq(config.pwm_freq);
    }
    for (int gpio=0; gpio < NUM_GPIO; gpio++ ) {
      if ( ( valid_gpio_mask & 1<<gpio ) && (config.pwm_gpio_enabled[gpio]) ) {
        pinMode(gpio, OUTPUT);
        if (config.pwm_gpio_invert[gpio]) {
          analogWrite(gpio, 1023);
        } else {
          analogWrite(gpio, 0);          
        }
      }
    }
  }
}

void handlePWM() {
  if ( config.pwm_enabled ) {
    for (int gpio=0; gpio < NUM_GPIO; gpio++ ) {
      if ( ( valid_gpio_mask & 1<<gpio ) && (config.pwm_gpio_enabled[gpio]) ) {
        int gpio_dmx = config.pwm_gpio_dmx[gpio];
        if (gpio_dmx < config.channel_count) {
#if defined (ESPS_MODE_PIXEL)
          uint16_t pwm_val = (config.pwm_gamma) ? GAMMA_TABLE[pixels.getData()[gpio_dmx]]>>6 : pixels.getData()[gpio_dmx]<<2;
#elif defined(ESPS_MODE_SERIAL)
          uint16_t pwm_val = (config.pwm_gamma) ? GAMMA_TABLE[serial.getData()[gpio_dmx]]>>6 : serial.getData()[gpio_dmx]<<2;
#endif
          if ( pwm_val != last_pwm[gpio]) {
            last_pwm[gpio] = pwm_val;
            if (config.pwm_gpio_invert[gpio]) {
              analogWrite(gpio, 1023-pwm_val);  // 0..1023 => 1023..0
            } else {
              analogWrite(gpio, pwm_val);       // 0..1023 => 0..1023
            }
          }
        }
      }
    }
  }
}
#endif

