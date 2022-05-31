/****************************************************************************
http://retro.moe/unijoysticle2

Copyright 2022 Ricardo Quesada

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
****************************************************************************/

/*
 * Based on SmallyMouse2 by Simon Inns
 * https://github.com/simoninns/SmallyMouse2
 */
#include "uni_mouse_quadrature.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <driver/gpio.h>
#include <driver/timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "uni_debug.h"

// Probably I could use a smaller divider, and only do "1 tick per 80us".
// That would work Ok except that it will loose resolution when we divide "128 steps by delta".
// APB clock runs at 80Mhz.
//   Option A:
//   80Mhz / 80 = 1Mhz = tick every 1us
// #define TIMER_DIVIDER (80)
// #define TICKS_PER_80US (80)  // How many ticks are in 80us
// #define ONE_SECOND (1000000)
//
//   Option B:
//   80Mhz / 6400 = 12500Hz = tick every 80us
#define TIMER_DIVIDER (80 * 80)
#define TICKS_PER_80US (1)  // How many ticks are in 80us
#define ONE_SECOND (12500)

#define TASK_TIMER_STACK_SIZE (1536)
#define TASK_TIMER_PRIO (10)

// Default scale factor for the mouse movement
#define DETAULT_SCALE_FACTOR (1)

enum direction {
    PHASE_DIRECTION_NEG,
    PHASE_DIRECTION_POS,
};

// A mouse has two encoders.
struct quadrature_state {
    // horizontal or vertical encoder?
    int encoder;
    // Current direction
    enum direction dir;
    // Current value
    int value;
    // Current quadrature phase
    int phase;

    // Which group timer/timer is being used
    int timer_group;
    timer_idx_t timer_idx;

    // GPIOs used
    struct uni_mouse_quadrature_encoder_gpios gpios;
};

static struct quadrature_state s_quadratures[UNI_MOUSE_QUADRATURE_PORT_MAX][UNI_MOUSE_QUADRATURE_ENCODER_MAX];
// Cache to prevent enabling/disabling timers that were already enabled/disabled
static bool timer_started[UNI_MOUSE_QUADRATURE_PORT_MAX];

static TaskHandle_t s_timer_tasks[UNI_MOUSE_QUADRATURE_PORT_MAX][UNI_MOUSE_QUADRATURE_ENCODER_MAX];

// "Scale factor" for mouse movement. To make the mouse move faster or slower.
// Bigger means slower movement.
static float s_scale_factor;

// NVS
static const char* STORAGE_NAMESPACE = "bp32";
static const char* NVS_KEY_SCALE_FACTOR = "mouse.scale";

static void process_quadrature(struct quadrature_state* q) {
    int a, b;

    if (q->value <= 0) {
        return;
    }
    q->value--;

    if (q->dir == PHASE_DIRECTION_NEG) {
        q->phase--;
        if (q->phase < 0)
            q->phase = 3;
    } else /* PHASE_DIRECTION_POS */ {
        q->phase++;
        if (q->phase > 3)
            q->phase = 0;
    }

    switch (q->phase) {
        case 0:
            a = 0;
            b = 0;
            break;
        case 1:
            a = 1;
            b = 0;
            break;
        case 2:
            a = 1;
            b = 1;
            break;
        case 3:
            a = 0;
            b = 1;
            break;
        default:
            loge("%s: invalid phase value: %d", __func__, q->phase);
            a = b = 0;
            break;
    }

    int gpio_a = q->gpios.a;
    int gpio_b = q->gpios.b;
    gpio_set_level(gpio_a, a);
    gpio_set_level(gpio_b, b);
    logd("value: %d, quadrature phase: %d, a=%d, b=%d (%d,%d)\n", q->value, q->phase, a, b, gpio_a, gpio_b);
}

// Don't be confused that is is just one task.
// Actually this callback is called from 4 different tasks.
static void timer_task(void* arg) {
    uint32_t a = (uint32_t)arg;
    uint16_t port_idx = (a >> 16);
    uint16_t encoder_idx = (a & 0xffff);

    while (true) {
        ulTaskNotifyTake(true, portMAX_DELAY);
        process_quadrature(&s_quadratures[port_idx][encoder_idx]);
    }
}

static bool IRAM_ATTR timer_handler(void* arg) {
    uint32_t a = (uint32_t)arg;
    uint16_t port_idx = (a >> 16);
    uint16_t encoder_idx = (a & 0xffff);

    BaseType_t higher_priority_task_woken = false;
    vTaskNotifyGiveFromISR(s_timer_tasks[port_idx][encoder_idx], &higher_priority_task_woken);
    return higher_priority_task_woken;
}

static void init_from_cpu_task() {
    // From ESP-IDF documentation:
    // "Register Timer interrupt handler, the handler is an ISR.
    // The handler will be attached to the same CPU core that this function is running on."

    // Create timers
    /* Select and initialize basic parameters of the timer */
    timer_config_t config = {
        .divider = TIMER_DIVIDER,
        .counter_dir = TIMER_COUNT_DOWN,
        .counter_en = TIMER_PAUSE,
        .alarm_en = TIMER_ALARM_EN,
        .auto_reload = TIMER_AUTORELOAD_EN,
    };

    for (int i = 0; i < UNI_MOUSE_QUADRATURE_PORT_MAX; i++) {
        for (int j = 0; j < UNI_MOUSE_QUADRATURE_ENCODER_MAX; j++) {
            uint32_t arg = (i << 16) | j;
            char name[32];
            ESP_ERROR_CHECK(timer_init(s_quadratures[i][j].timer_group, s_quadratures[i][j].timer_idx, &config));
            timer_set_counter_value(s_quadratures[i][j].timer_group, s_quadratures[i][j].timer_idx, ONE_SECOND * 60);
            timer_isr_callback_add(s_quadratures[i][j].timer_group, s_quadratures[i][j].timer_idx, timer_handler,
                                   (void*)arg, 0);
            // Don't start timer automatically. They should be started on demand.
            // timer_start(s_quadratures[i][j].timer_group, s_quadratures[i][j].timer_idx);

            // Create timer tasks
            sprintf(name, "bp.quad.timer%d%c", i, (j == 0) ? 'H' : 'V');
            xTaskCreatePinnedToCore(timer_task, name, TASK_TIMER_STACK_SIZE, (void*)arg, TASK_TIMER_PRIO,
                                    &s_timer_tasks[i][j], xPortGetCoreID());
        }
    }

    // Kill itself
    vTaskDelete(NULL);
}

static void process_update(struct quadrature_state* q, int32_t delta) {
    uint64_t units;
    int32_t abs_delta = (delta < 0) ? -delta : delta;

    if (delta != 0) {
        // Hack to make the movement smoother with "small deltas". It guarantees that
        // at least two phases (out of 4) of the quadrature will be run.
        // if (abs_delta < 2)
        //     abs_delta = 2;
        /* Don't update the phase, it should start from the previous phase */
        q->value = abs_delta;
        q->dir = (delta < 0) ? PHASE_DIRECTION_NEG : PHASE_DIRECTION_POS;

        // SmallyMouse2 mentions that 100-120 reports are recevied per second.
        // According to my test they are ~90, which is in the same order.
        // For simplicity, I'll use 100. It means that, at most, reports are received
        // every 10ms (1 second / 100 reports = 10ms per report).
        //
        // "delta" is a "somewhat normalized" value that goes from 0 to 127.
        // So we should split 10 milliseconds (ms) in 128 steps = ~80 microseconds (us).
        // That's good because the minimum "ISR" in ESP32 is 50us.
        // So the ESP32 timer is configured as:
        // - down timer
        // - ticks every 1us
        // - when it reaches 0, triggers the ISR
        //
        // But a quadrature has 4 states (hence the name). So takes 4 "ticks" to have
        // complete "state.", which is represented with "s_scale_factor",
        // kind of "hand tuned" so that the mice movement feels "good" (to me).
        //
        // The smaller "units" is, the faster the mouse moves.
        //
        // But in order to avoid a "division" in the mouse driver, and a multiplication here,
        // (which will  loose precision), we just use a "s_scale_factor" of 1 instead of 4,
        // and we don't divide by 4 here.
        // Alternative: Do not divide the time, and use a constant "tick" time. But if we do so,
        // the movement will have "jank".
        // Perhaps for small deltas we can have a predefined "unit time"
        float max_ticks = 128 * TICKS_PER_80US;
        float delta_f = abs_delta;
        float units_f = (max_ticks / delta_f) * s_scale_factor;
        if (units_f < TICKS_PER_80US)
            units_f = TICKS_PER_80US;
        units = roundf(units_f);
    } else {
        // If there is no update, set timer to update less frequently
        units = ONE_SECOND * 60;
    }
    timer_set_counter_value(q->timer_group, q->timer_idx, units);
}

void uni_mouse_quadrature_init(int cpu_id) {
    memset(s_quadratures, 0, sizeof(s_quadratures));

    for (int i = 0; i < UNI_MOUSE_QUADRATURE_PORT_MAX; i++) {
        timer_started[i] = false;
        for (int j = 0; j < UNI_MOUSE_QUADRATURE_ENCODER_MAX; j++) {
            s_quadratures[i][j].timer_group = TIMER_GROUP_0 + i;
            s_quadratures[i][j].timer_idx = TIMER_0 + j;
        }
    }

    // Default value that can be overriden from the console
    s_scale_factor = uni_mouse_quadrature_get_scale_factor();

    // Create tasks
    xTaskCreatePinnedToCore(init_from_cpu_task, "uni.init_timers", TASK_TIMER_STACK_SIZE, NULL, TASK_TIMER_PRIO, NULL,
                            cpu_id);
}

void uni_mouse_quadrature_setup_port(int port_idx,
                                     struct uni_mouse_quadrature_encoder_gpios h,
                                     struct uni_mouse_quadrature_encoder_gpios v) {
    if (port_idx < 0 || port_idx >= UNI_MOUSE_QUADRATURE_PORT_MAX) {
        loge("%s: Invalid port idx=%d\n", __func__, port_idx);
        return;
    }
    s_quadratures[port_idx][UNI_MOUSE_QUADRATURE_ENCODER_H].gpios = h;
    s_quadratures[port_idx][UNI_MOUSE_QUADRATURE_ENCODER_V].gpios = v;
}

void uni_mouse_quadrature_deinit() {
    // Stop the timers
    for (int i = 0; i < UNI_MOUSE_QUADRATURE_PORT_MAX; i++) {
        for (int j = 0; j < UNI_MOUSE_QUADRATURE_ENCODER_MAX; j++) {
            timer_deinit(s_quadratures[i][j].timer_group, s_quadratures[i][j].timer_idx);
            // Delete the tasks
            vTaskDelete(s_timer_tasks[i][j]);
            s_timer_tasks[i][j] = NULL;
        }
    }
}

void uni_mouse_quadrature_start(int port_idx) {
    if (port_idx < 0 || port_idx >= UNI_MOUSE_QUADRATURE_PORT_MAX) {
        loge("%s: Invalid port idx=%d\n", __func__, port_idx);
        return;
    }

    if (timer_started[port_idx])
        return;

    for (int j = 0; j < UNI_MOUSE_QUADRATURE_ENCODER_MAX; j++)
        timer_start(s_quadratures[port_idx][j].timer_group, s_quadratures[port_idx][j].timer_idx);

    timer_started[port_idx] = true;
}

void uni_mouse_quadrature_pause(int port_idx) {
    if (port_idx < 0 || port_idx >= UNI_MOUSE_QUADRATURE_PORT_MAX) {
        loge("%s: Invalid port idx=%d\n", __func__, port_idx);
        return;
    }

    if (!timer_started[port_idx])
        return;

    for (int j = 0; j < UNI_MOUSE_QUADRATURE_ENCODER_MAX; j++)
        timer_pause(s_quadratures[port_idx][j].timer_group, s_quadratures[port_idx][j].timer_idx);

    timer_started[port_idx] = false;
}

// Should be called everytime that mouse report is received.
void uni_mouse_quadrature_update(int port_idx, int32_t dx, int32_t dy) {
    if (port_idx < 0 || port_idx >= UNI_MOUSE_QUADRATURE_PORT_MAX) {
        loge("%s: Invalid port idx=%d\n", __func__, port_idx);
        return;
    }
    process_update(&s_quadratures[port_idx][UNI_MOUSE_QUADRATURE_ENCODER_H], dx);
    // Invert delta Y so that mouse goes the the right direction.
    // This is based on emperic evidence. Also, it seems that SmallyMouse is doing the same thing
    process_update(&s_quadratures[port_idx][UNI_MOUSE_QUADRATURE_ENCODER_V], -dy);
}

void uni_mouse_quadrature_set_scale_factor(float scale) {
    nvs_handle_t nvs_handle;
    esp_err_t err;
    uint32_t* scale_alias = (uint32_t*)&scale;

    // Update runtime value
    s_scale_factor = scale;

    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        loge("Could not open NVS storage\n");
        return;
    }

    // float not supported. Casting it to u32
    err = nvs_set_u32(nvs_handle, NVS_KEY_SCALE_FACTOR, *scale_alias);
    if (err != ESP_OK) {
        loge("Could not save scale factor in NVS\n");
        goto out;
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        loge("Could commit scale factor in NVS\n");
        /* fallthrough */
    } else {
        logi("Ok");
    }

out:
    nvs_close(nvs_handle);
}

float uni_mouse_quadrature_get_scale_factor() {
    nvs_handle_t nvs_handle;
    esp_err_t err;
    float scale_factor;

    err = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK)
        return DETAULT_SCALE_FACTOR;

    // float not supported. Casting it to u32
    err = nvs_get_u32(nvs_handle, NVS_KEY_SCALE_FACTOR, (uint32_t*)&scale_factor);
    if (err != ESP_OK) {
        scale_factor = DETAULT_SCALE_FACTOR;
        /* falltrhough */
    }

    nvs_close(nvs_handle);
    return scale_factor;
}