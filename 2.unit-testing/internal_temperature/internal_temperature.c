/*
 * Pico 2 W — RP2350 internal temperature sensor
 *
 * The RP2350 has a temperature sensor wired to the last ADC channel (channel 4
 * on the RP2350A package used by the Pico 2 W). It is not exposed on a pin —
 * it is internal to the die, so it measures *chip* temperature, which sits a
 * few degrees above ambient because the core is warming it.
 *
 * Conversion comes from the RP2350 datasheet:
 *
 *     T = 27 - (V - 0.706) / 0.001721
 *
 * where V is the sensor voltage. 0.706 V is the nominal reading at 27 degC and
 * the sensor falls by 1.721 mV per degree.
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"

#define ADC_VREF        3.3f      // ADC reference voltage on the Pico 2 W
#define ADC_RESOLUTION  (1 << 12) // 12-bit ADC: 0..4095
#define SAMPLES         16        // averaged per reading, to settle the noise
#define PERIOD_MS       1000

// Datasheet constants
#define T_AT_706MV      27.0f
#define VOLTS_PER_DEGC  0.001721f
#define V_AT_27C        0.706f

static float adc_to_celsius(float raw_average)
{
    float voltage = raw_average * ADC_VREF / ADC_RESOLUTION;
    return T_AT_706MV - (voltage - V_AT_27C) / VOLTS_PER_DEGC;
}

int main(void)
{
    stdio_init_all();
    sleep_ms(3000);   // let the UART settle so the first lines are seen

    printf("RP2350 internal temperature sensor\n");
    printf("ADC channel %d, %d-bit, Vref %.1f V, %d samples averaged\n\n",
           ADC_TEMPERATURE_CHANNEL_NUM, 12, ADC_VREF, SAMPLES);

    adc_init();                          // power up the ADC block
    adc_set_temp_sensor_enabled(true);   // enable the on-die sensor
    adc_select_input(ADC_TEMPERATURE_CHANNEL_NUM);

    // The sensor needs a moment after being enabled before it reads true.
    sleep_ms(10);

    float min_c =  1000.0f;
    float max_c = -1000.0f;
    unsigned long reading = 0;

    while (true)
    {
        // Average several conversions: a single 12-bit sample jitters by a
        // couple of LSB, which is a few tenths of a degree here.
        uint32_t sum = 0;
        for (int i = 0; i < SAMPLES; i++) {
            sum += adc_read();
        }
        float raw_avg = (float)sum / SAMPLES;

        float voltage = raw_avg * ADC_VREF / ADC_RESOLUTION;
        float celsius = adc_to_celsius(raw_avg);
        float fahrenheit = celsius * 9.0f / 5.0f + 32.0f;

        if (celsius < min_c) min_c = celsius;
        if (celsius > max_c) max_c = celsius;

        printf("[%5lu] raw %6.1f  %.4f V  %6.2f degC  %6.2f degF   "
               "(min %.2f / max %.2f)\n",
               ++reading, raw_avg, voltage, celsius, fahrenheit, min_c, max_c);

        sleep_ms(PERIOD_MS);
    }
}
