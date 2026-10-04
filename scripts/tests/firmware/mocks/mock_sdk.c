#include <assert.h>
#include "mock_sdk.h"

spi_inst_t mock_spi_instances[2];
dma_hw_t mock_dma_hw;
adc_hw_t mock_adc_hw;
pwm_hw_t mock_pwm_hw;
bool mock_fail_mutex, mock_spi_stuck;
bool mock_dma_complete = true;
int mock_scheduler_state = taskSCHEDULER_RUNNING;
unsigned mock_gpio_function[30], mock_gpio_level[30];
unsigned mock_adc_last_input, mock_adc_round_robin, mock_spi_reset_count;
uint64_t mock_time_us;
SemaphoreHandle_t mock_pwm_mutex[12];
void (*mock_before_mutex_take)(void);
static mock_sem_t semaphores[16];
static unsigned sem_count, dma_count, spi_busy_polls, critical_depth;
UBaseType_t mock_enter_critical(void) { return critical_depth++; }
void mock_exit_critical(void) { assert(critical_depth > 0); critical_depth--; }
static uint32_t irq_enabled, irq_pending;
static irq_handler_t dma_handler;
static uint adc_input, adc_dma_channel;
static uint16_t *adc_buffer;
static bool fifo_enabled, fifo_pending;
static uint16_t fifo_sample;

TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)(uintptr_t)1; }
int xTaskGetSchedulerState(void) { return mock_scheduler_state; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    assert(sem_count < count_of(semaphores));
    semaphores[sem_count] = (mock_sem_t){.count = 1, .mutex = true};
    return &semaphores[sem_count++];
}
SemaphoreHandle_t xSemaphoreCreateBinary(void) {
    assert(sem_count < count_of(semaphores));
    semaphores[sem_count] = (mock_sem_t){0};
    return &semaphores[sem_count++];
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks) {
    assert(sem != NULL);
    if (sem->mutex && mock_before_mutex_take) {
        void (*hook)(void) = mock_before_mutex_take;
        mock_before_mutex_take = NULL;
        hook();
    }
    if (sem->mutex && mock_fail_mutex) return pdFALSE;
    if (sem->count == 0) {
        if (ticks != portMAX_DELAY) mock_time_us += (uint64_t)ticks * 1000;
        return pdFALSE;
    }
    sem->count--;
    if (sem->mutex) sem->holder = xTaskGetCurrentTaskHandle();
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem) {
    assert(sem != NULL);
    sem->count = 1; sem->holder = NULL; return pdTRUE;
}
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t sem, BaseType_t *woken) {
    *woken = pdFALSE; return xSemaphoreGive(sem);
}
unsigned uxSemaphoreGetCount(SemaphoreHandle_t sem) { return sem->count; }
TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t sem) { return sem->holder; }
uint64_t time_us_64(void) { mock_time_us += 100; return mock_time_us; }
void tight_loop_contents(void) {}
void gpio_init(uint pin) { mock_gpio_function[pin] = 0; }
void gpio_set_dir(uint pin, bool out) { (void)pin; (void)out; }
void gpio_put(uint pin, bool value) { mock_gpio_level[pin] = value; }
void gpio_set_function(uint pin, uint function) { mock_gpio_function[pin] = function; }
void irq_set_exclusive_handler(uint irq, irq_handler_t handler) { (void)irq; dma_handler = handler; }
void irq_set_priority(uint irq, uint priority) { (void)irq; (void)priority; }
void irq_set_enabled(uint irq, bool enabled) { (void)irq; (void)enabled; }
uint spi_init(spi_inst_t *inst, uint baudrate) { inst->baudrate = baudrate; return baudrate; }
void spi_deinit(spi_inst_t *inst) {
    (void)inst; spi_busy_polls = 0; mock_spi_stuck = false; mock_spi_reset_count++;
}
uint spi_set_baudrate(spi_inst_t *inst, uint baudrate) { inst->baudrate = baudrate; return baudrate; }
void spi_set_format(spi_inst_t *inst, uint bits, uint cpol, uint cpha, uint order) {
    (void)inst; (void)bits; (void)cpol; (void)cpha; (void)order;
}
bool spi_is_busy(spi_inst_t *inst) {
    (void)inst;
    if (mock_spi_stuck) return true;
    if (spi_busy_polls) { spi_busy_polls--; return true; }
    return false;
}
bool spi_is_readable(spi_inst_t *inst) { (void)inst; return false; }
spi_hw_t *spi_get_hw(spi_inst_t *inst) { return &inst->hw; }
uint spi_get_dreq(spi_inst_t *inst, bool tx) { (void)inst; return tx ? 0 : 1; }
dma_channel_hw_t *dma_channel_hw_addr(uint channel) { return &dma_hw->ch[channel]; }
int dma_claim_unused_channel(bool required) { (void)required; return (int)dma_count++; }
void dma_channel_unclaim(uint channel) { (void)channel; }
dma_channel_config dma_channel_get_default_config(uint channel) { (void)channel; return (dma_channel_config){0}; }
void channel_config_set_transfer_data_size(dma_channel_config *cfg, uint size) { cfg->bits = (uint8_t)size; }
void channel_config_set_read_increment(dma_channel_config *cfg, bool increment) { (void)cfg; (void)increment; }
void channel_config_set_write_increment(dma_channel_config *cfg, bool increment) { (void)cfg; (void)increment; }
void channel_config_set_ring(dma_channel_config *cfg, bool write, uint bits) { (void)cfg; (void)write; (void)bits; }
void channel_config_set_dreq(dma_channel_config *cfg, uint dreq) { cfg->dreq = (uint8_t)dreq; }
void channel_config_set_high_priority(dma_channel_config *cfg, bool priority) { (void)cfg; (void)priority; }
void dma_channel_configure(uint channel, const dma_channel_config *cfg, volatile void *dst,
                           const volatile void *src, uint32_t count, bool start) {
    (void)cfg; (void)count; (void)start;
    dma_hw->ch[channel].write_addr = (uintptr_t)dst;
    if (src == &adc_hw->fifo) { adc_buffer = (uint16_t *)dst; adc_dma_channel = channel; }
}
void dma_start_channel_mask(uint32_t mask) {
    spi_busy_polls = 4;
    if (mock_dma_complete) {
        irq_pending |= mask;
        if ((irq_pending & irq_enabled) && dma_handler) dma_handler();
    }
}
void dma_channel_start(uint channel) { dma_start_channel_mask(1u << channel); }
void dma_channel_abort(uint channel) { (void)channel; }
void dma_irqn_set_channel_enabled(uint irq, uint channel, bool enabled) {
    (void)irq;
    if (enabled) irq_enabled |= 1u << channel; else irq_enabled &= ~(1u << channel);
}
void dma_irqn_acknowledge_channel(uint irq, uint channel) { (void)irq; irq_pending &= ~(1u << channel); }
bool dma_irqn_get_channel_status(uint irq, uint channel) { (void)irq; return (irq_pending & irq_enabled & (1u << channel)) != 0; }
uint32_t dma_encode_endless_transfer_count(void) { return UINT32_MAX; }
void adc_init(void) { fifo_pending = false; }
void adc_gpio_init(uint pin) { (void)pin; }
void adc_set_temp_sensor_enabled(bool enabled) { (void)enabled; }
void adc_fifo_setup(bool enabled, bool dreq, uint threshold, bool err, bool shift) {
    (void)dreq; (void)threshold; (void)err; (void)shift; fifo_enabled = enabled;
}
void adc_set_clkdiv(float div) { (void)div; }
void adc_set_round_robin(uint mask) { mock_adc_round_robin = mask; }
void adc_select_input(uint input) { adc_input = input; }
static uint16_t adc_sample(uint input) {
    return input == 0 ? 1111 : input == 1 ? 2222 : input == 2 ? 3333 : 900;
}
void adc_run(bool run) {
    if (!run) return;
    assert(adc_buffer != NULL);
    // FIFO 中的手动样本先被 DMA 取走，再开始双通道连续转换。
    for (unsigned i = 0; i < 4; i++) {
        if (fifo_pending) { adc_buffer[i] = fifo_sample; fifo_pending = false; }
        else {
            adc_buffer[i] = adc_sample(adc_input);
            if (mock_adc_round_robin == 3) adc_input = adc_input == 0 ? 1 : 0;
        }
    }
    dma_hw->ch[adc_dma_channel].write_addr = (uintptr_t)(adc_buffer + 4);
}
void adc_fifo_drain(void) { fifo_pending = false; }
uint16_t adc_read(void) {
    assert(mock_adc_round_robin == 0);
    mock_adc_last_input = adc_input;
    const uint16_t value = adc_sample(adc_input);
    if (fifo_enabled) { fifo_sample = value; fifo_pending = true; }
    return value;
}
uint32_t clock_get_hz(uint clock) { (void)clock; return 150000000; }
uint pwm_gpio_to_slice_num(uint pin) { return (pin >> 1u) & 7u; }
uint pwm_gpio_to_channel(uint pin) { return pin & 1u; }
pwm_config pwm_get_default_config(void) { return (pwm_config){0}; }
void pwm_config_set_clkdiv_int_frac(pwm_config *cfg, uint8_t integer, uint8_t fraction) { (void)cfg; (void)integer; (void)fraction; }
void pwm_config_set_wrap(pwm_config *cfg, uint16_t wrap) { cfg->wrap = wrap; }
void pwm_config_set_phase_correct(pwm_config *cfg, bool enabled) { (void)cfg; (void)enabled; }
void pwm_config_set_output_polarity(pwm_config *cfg, bool a, bool b) { (void)cfg; (void)a; (void)b; }
static void check_pwm_lock(uint slice) {
    if (mock_pwm_mutex[slice]) assert(mock_pwm_mutex[slice]->holder == xTaskGetCurrentTaskHandle());
}
void pwm_init(uint slice, const pwm_config *cfg, bool start) { (void)start; pwm_hw->slice[slice].top = cfg->wrap; }
void pwm_set_chan_level(uint slice, uint channel, uint16_t level) {
    check_pwm_lock(slice);
    const uint shift = channel * 16u;
    pwm_hw->slice[slice].cc = (pwm_hw->slice[slice].cc & ~(0xFFFFu << shift)) | ((uint32_t)level << shift);
}
void pwm_set_enabled(uint slice, bool enabled) { (void)enabled; check_pwm_lock(slice); }
void pwm_set_clkdiv_int_frac(uint slice, uint8_t integer, uint8_t fraction) { (void)integer; (void)fraction; check_pwm_lock(slice); }
void pwm_set_wrap(uint slice, uint16_t wrap) { check_pwm_lock(slice); pwm_hw->slice[slice].top = wrap; }
