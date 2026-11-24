/*
 * OsitoK - Input subsystem
 *
 * Polls joystick X axis (ADC) and button (GPIO12) at ~50Hz.
 * Generates direction events with dead zone and button events with debounce.
 */

#include "drivers/input.h"
#include "drivers/adc.h"
#include "drivers/gpio.h"
#include "kernel/task.h"

extern "C" {

/* ====== Event ring buffer ====== */

static input_event_t event_queue[INPUT_QUEUE_SIZE];
static volatile uint8_t eq_head = 0;
static volatile uint8_t eq_tail = 0;

static void event_push(input_event_t ev)
{
    uint8_t next = (eq_head + 1) & (INPUT_QUEUE_SIZE - 1);
    if (next == eq_tail)
        return;  /* queue full, drop event */
    event_queue[eq_head] = ev;
    eq_head = next;
}

input_event_t input_poll(void)
{
    if (eq_head == eq_tail)
        return INPUT_NONE;
    input_event_t ev = event_queue[eq_tail];
    eq_tail = (eq_tail + 1) & (INPUT_QUEUE_SIZE - 1);
    return ev;
}

/* ====== State tracking ====== */

static uint16_t last_x_raw = 512;     /* center */
static uint8_t  last_dir = 0;         /* 0=center, 1=left, 2=right */
static uint8_t  btn_state = 0;        /* 0=released, 1=pressed */
static uint8_t  btn_debounce = 0;     /* debounce counter */
static uint8_t  btn_raw_last = 0;     /* last raw button reading */

void input_init(void)
{
    /* Initialize ADC for joystick X axis */
    adc_init();

    /* Configure button pin as input with pull-up (active low) */
    gpio_mode(INPUT_BTN_PIN, GPIO_MODE_INPUT);
    gpio_pullup(INPUT_BTN_PIN, true);

    /* Reset state */
    eq_head = 0;
    eq_tail = 0;
    last_x_raw = 512;
    last_dir = 0;
    btn_state = 0;
    btn_debounce = 0;
    btn_raw_last = 0;
}

void input_update(void)
{
    /* ---- Joystick X axis ---- */
    last_x_raw = adc_read();

    uint8_t dir = 0;
    if (last_x_raw < INPUT_DEAD_LOW)
        dir = 1;  /* left */
    else if (last_x_raw > INPUT_DEAD_HIGH)
        dir = 2;  /* right */

    if (dir != last_dir) {
        if (dir == 1)
            event_push(INPUT_LEFT);
        else if (dir == 2)
            event_push(INPUT_RIGHT);
        /* center transitions don't generate events */
        last_dir = dir;
    }

    /* ---- Button with debounce ---- */
    uint8_t raw_btn = gpio_read(INPUT_BTN_PIN) ? 0 : 1;  /* active low */

    if (raw_btn != btn_raw_last) {
        btn_debounce = 0;
        btn_raw_last = raw_btn;
    } else {
        if (btn_debounce < INPUT_DEBOUNCE)
            btn_debounce++;
    }

    if (btn_debounce >= INPUT_DEBOUNCE && raw_btn != btn_state) {
        btn_state = raw_btn;
        event_push(btn_state ? INPUT_PRESS : INPUT_RELEASE);
    }
}

uint32_t input_get_state(void)
{
    return (uint32_t)last_x_raw | ((uint32_t)btn_state << 16);
}

/* ====== Input task ====== */

void input_task(void *arg)
{
    (void)arg;

    for (;;) {
        input_update();
        task_delay_ticks(2);  /* ~50Hz (every 2 ticks at 100Hz) */
    }
}

} /* extern "C" */
