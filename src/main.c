/*
*/
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_sleep.h"

#include "sys/time.h"
#include <math.h>

#define LED 2 // LED connected to GPIO2
#define DEFAULT_VREF    1100        //Use adc2_vref_to_gpio() to obtain a better estimate
#define NO_OF_SAMPLES   150          //Multisampling
#define DEFAULT_SENSOR_OUTPUT   1630 // Vref / 2 
#define SAMPLES 50
#define BPM_WINDOW_SIZE 4

static esp_adc_cal_characteristics_t *adc_chars;

static const adc_channel_t channel = ADC1_CHANNEL_0;     //GPIO36
static const adc_bits_width_t width = ADC_WIDTH_BIT_12;

static const adc_atten_t atten = ADC_ATTEN_DB_11;
static const adc_unit_t unit = ADC_UNIT_1;

int get_bpm();


static void check_efuse(void)
{
    //Check if TP is burned into eFuse
    if (esp_adc_cal_check_efuse(ESP_ADC_CAL_VAL_EFUSE_TP) == ESP_OK) {
        printf("eFuse Two Point: Supported\n");
    } else {
        printf("eFuse Two Point: NOT supported\n");
    }
    //Check Vref is burned into eFuse
    if (esp_adc_cal_check_efuse(ESP_ADC_CAL_VAL_EFUSE_VREF) == ESP_OK) {
        printf("eFuse Vref: Supported\n");
    } else {
        printf("eFuse Vref: NOT supported\n");
    }
}


static void print_char_val_type(esp_adc_cal_value_t val_type)
{
    if (val_type == ESP_ADC_CAL_VAL_EFUSE_TP) {
        printf("Characterized using Two Point Value\n");
    } else if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
        printf("Characterized using eFuse Vref\n");
    } else {
        printf("Characterized using Default Vref\n");
    }
}

uint32_t read_sensor_voltage() {
    static int sum = 0;
    static int window[NO_OF_SAMPLES];
    static int index;

    uint32_t tmp_adc_reading = 0;
    uint32_t voltage;

    for (int i =0; i < NO_OF_SAMPLES; i++) {
        sum = sum - window[index];       // Remove the oldest entry from the sum
        tmp_adc_reading = adc1_get_raw((adc1_channel_t)channel);
        voltage = esp_adc_cal_raw_to_voltage(tmp_adc_reading, adc_chars);

        window[index] = voltage;           // Add the newest reading to the window
        sum = sum + voltage;                 // Add the newest reading to the sum
        index = (index+1) % NO_OF_SAMPLES;   // Increment the index, and wrap to 0 if it exceeds the window size
    }

    int averaged = sum / NO_OF_SAMPLES; 
    
    int output = (averaged - DEFAULT_SENSOR_OUTPUT < 0 || averaged - DEFAULT_SENSOR_OUTPUT > 150) ? 0 : averaged - DEFAULT_SENSOR_OUTPUT;

    return output;
}


void app_main(void)
{
    //Check if Two Point or Vref are burned into eFuse
    check_efuse();

    //Configure ADC
    adc1_config_width(width);
    adc1_config_channel_atten(channel, atten);

    //Characterize ADC
    adc_chars = calloc(1, sizeof(esp_adc_cal_characteristics_t));
    esp_adc_cal_value_t val_type = esp_adc_cal_characterize(unit, atten, width, DEFAULT_VREF, adc_chars);
    print_char_val_type(val_type);

    static int bpm_sum = 0;
    static int bpm_window[BPM_WINDOW_SIZE] = {0};
    static int idx = 0;

    uint32_t curr_bpm;
    int averaged_pulse;
    //Continuou sly sample ADC1
    while (1) {
        for (int i =0; i < BPM_WINDOW_SIZE; i++) {
            bpm_sum = bpm_sum - bpm_window[idx];            // Remove the oldest entry from the sum
            
            curr_bpm = get_bpm();
            printf("Curr bpm: %d\n", curr_bpm);

            if (curr_bpm < 40) {
                do {
                    curr_bpm = get_bpm();
                    printf("Curr bpm: %d\n", curr_bpm);
                    if (curr_bpm < 30 && bpm_sum != 0) {
                        bpm_sum = 0;
                        for (int p = 0; p < BPM_WINDOW_SIZE; p++)
                            bpm_window[p] = 0;
                        printf("BPM 0\n");    
                    }
                } while(curr_bpm < 30);
            }

            bpm_window[idx] = curr_bpm;                     // Add the newest reading to the window
            bpm_sum = bpm_sum + curr_bpm;                   // Add the newest reading to the sum
            idx = (idx+1) % BPM_WINDOW_SIZE;                              // Increment the index, and wrap to 0 if it exceeds the window size

            int non_zero = 0;
            for (int o = 0; o < BPM_WINDOW_SIZE; o++) {
                if (bpm_window[o] != 0)
                    non_zero++;
            }

            averaged_pulse = bpm_sum / non_zero;
      
            printf("BPM %d\n", averaged_pulse);
        }

    }
}

int get_bpm() {
    int samples_number = (esp_random() % (65 - 55 + 1)) + 55;

    int pulse[samples_number];
    int beats = 0;
    
    struct timeval start, stop;
    double secs;
    beats = 0;
    gettimeofday(&start, NULL);
    for(int j = 0; j < samples_number; j++) {
        pulse[j] = read_sensor_voltage();
        vTaskDelay((esp_random() % 20) + 1);
        // printf("%d;%d\n", j, pulse[j]);
    }
    gettimeofday(&stop, NULL);
    secs = (double)(stop.tv_usec - start.tv_usec) / 1000000 + (double)(stop.tv_sec - start.tv_sec);

    int peak_value = -1;
    int peak_index = -1;
    int sum = 0;

    for(int i=0; i< samples_number; i++) {
        sum = sum + pulse[i];
    }

    int baseline = sum / samples_number;

    for (int k = 0; k < samples_number; k++) {
        if(pulse[k] > baseline && pulse[k] - baseline > 5) {
            if (peak_value == -1 || pulse[k] > peak_value) {
                peak_index = k;
                peak_value = pulse[k];
            }
        } else if (pulse[k] < baseline && peak_index != -1) {
            beats++;
            gpio_set_level(LED, 1);
            peak_index = -1;
            peak_value = -1;
            gpio_set_level(LED, 0);
        }
    }
    if (peak_index != -1 && beats != 0) {
        beats++;
        gpio_set_level(LED, 1);
        gpio_set_level(LED, 0);
    }

    int bpm = round(((60.0 / secs) * beats));
    return bpm;
}
