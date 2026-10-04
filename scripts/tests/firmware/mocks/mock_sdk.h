#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef unsigned int uint;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef void *TaskHandle_t;
typedef void (*irq_handler_t)(void);
typedef struct { unsigned count; bool mutex; TaskHandle_t holder; } mock_sem_t;
typedef mock_sem_t *SemaphoreHandle_t;
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portYIELD_FROM_ISR(x) ((void)(x))
#define taskENTER_CRITICAL() ((void)mock_enter_critical())
#define taskEXIT_CRITICAL() mock_exit_critical()
#define taskENTER_CRITICAL_FROM_ISR() mock_enter_critical()
#define taskEXIT_CRITICAL_FROM_ISR(saved) ((void)(saved), mock_exit_critical())
UBaseType_t mock_enter_critical(void);
void mock_exit_critical(void);
#define taskSCHEDULER_NOT_STARTED 0
#define taskSCHEDULER_RUNNING 2
#define count_of(a) (sizeof(a) / sizeof((a)[0]))
#define GPIO_OUT true
#define GPIO_FUNC_SPI 1
#define GPIO_FUNC_PWM 4
#define DMA_IRQ_0 0
#define DMA_SIZE_8 0
#define DMA_SIZE_16 1
#define DREQ_ADC 36
#define ADC_TEMPERATURE_CHANNEL_NUM 4
#define PWM_CHAN_A 0
#define PWM_CHAN_B 1
#define SPI_CPOL_0 0
#define SPI_CPOL_1 1
#define SPI_CPHA_0 0
#define SPI_CPHA_1 1
#define SPI_MSB_FIRST 0
#define SPI_SSPICR_RORIC_BITS 1
#define clk_sys 0

typedef struct { uint32_t dr, icr; } spi_hw_t;
typedef struct { spi_hw_t hw; uint32_t baudrate; } spi_inst_t;
extern spi_inst_t mock_spi_instances[2];
#define spi0 (&mock_spi_instances[0])
#define spi1 (&mock_spi_instances[1])
typedef struct { uintptr_t write_addr; } dma_channel_hw_t;
typedef struct { dma_channel_hw_t ch[16]; } dma_hw_t;
extern dma_hw_t mock_dma_hw;
#define dma_hw (&mock_dma_hw)
typedef struct { uint32_t fifo; } adc_hw_t;
extern adc_hw_t mock_adc_hw;
#define adc_hw (&mock_adc_hw)
typedef struct { uint32_t cc, top; } pwm_slice_hw_t;
typedef struct { pwm_slice_hw_t slice[12]; } pwm_hw_t;
extern pwm_hw_t mock_pwm_hw;
#define pwm_hw (&mock_pwm_hw)
typedef struct { uint8_t bits; uint8_t dreq; } dma_channel_config;
typedef struct { uint16_t wrap; } pwm_config;

extern bool mock_fail_mutex, mock_dma_complete, mock_spi_stuck;
extern int mock_scheduler_state;
extern unsigned mock_gpio_function[30], mock_gpio_level[30];
extern unsigned mock_adc_last_input, mock_adc_round_robin;
extern unsigned mock_spi_reset_count;
extern uint64_t mock_time_us;
extern SemaphoreHandle_t mock_pwm_mutex[12];
extern void (*mock_before_mutex_take)(void);

SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem);
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t sem, BaseType_t *woken);
unsigned uxSemaphoreGetCount(SemaphoreHandle_t sem);
TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t sem);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
int xTaskGetSchedulerState(void);
uint64_t time_us_64(void);
void tight_loop_contents(void);
void gpio_init(uint pin);
void gpio_set_dir(uint pin, bool out);
void gpio_put(uint pin, bool value);
void gpio_set_function(uint pin, uint function);
void irq_set_exclusive_handler(uint irq, irq_handler_t handler);
void irq_set_priority(uint irq, uint priority);
void irq_set_enabled(uint irq, bool enabled);
uint spi_init(spi_inst_t *inst, uint baudrate);
void spi_deinit(spi_inst_t *inst);
uint spi_set_baudrate(spi_inst_t *inst, uint baudrate);
void spi_set_format(spi_inst_t *inst, uint bits, uint cpol, uint cpha, uint order);
bool spi_is_busy(spi_inst_t *inst);
bool spi_is_readable(spi_inst_t *inst);
spi_hw_t *spi_get_hw(spi_inst_t *inst);
uint spi_get_dreq(spi_inst_t *inst, bool tx);
dma_channel_hw_t *dma_channel_hw_addr(uint channel);
int dma_claim_unused_channel(bool required);
void dma_channel_unclaim(uint channel);
dma_channel_config dma_channel_get_default_config(uint channel);
void channel_config_set_transfer_data_size(dma_channel_config *cfg, uint size);
void channel_config_set_read_increment(dma_channel_config *cfg, bool increment);
void channel_config_set_write_increment(dma_channel_config *cfg, bool increment);
void channel_config_set_ring(dma_channel_config *cfg, bool write, uint bits);
void channel_config_set_dreq(dma_channel_config *cfg, uint dreq);
void channel_config_set_high_priority(dma_channel_config *cfg, bool priority);
void dma_channel_configure(uint channel, const dma_channel_config *cfg, volatile void *dst,
                           const volatile void *src, uint32_t count, bool start);
void dma_channel_start(uint channel);
void dma_start_channel_mask(uint32_t mask);
void dma_channel_abort(uint channel);
void dma_irqn_set_channel_enabled(uint irq, uint channel, bool enabled);
void dma_irqn_acknowledge_channel(uint irq, uint channel);
bool dma_irqn_get_channel_status(uint irq, uint channel);
uint32_t dma_encode_endless_transfer_count(void);
void adc_init(void);
void adc_gpio_init(uint pin);
void adc_set_temp_sensor_enabled(bool enabled);
void adc_fifo_setup(bool enabled, bool dreq, uint threshold, bool err, bool shift);
void adc_set_clkdiv(float div);
void adc_set_round_robin(uint mask);
void adc_select_input(uint input);
void adc_run(bool run);
void adc_fifo_drain(void);
uint16_t adc_read(void);
uint32_t clock_get_hz(uint clock);
uint pwm_gpio_to_slice_num(uint pin);
uint pwm_gpio_to_channel(uint pin);
pwm_config pwm_get_default_config(void);
void pwm_config_set_clkdiv_int_frac(pwm_config *cfg, uint8_t integer, uint8_t fraction);
void pwm_config_set_wrap(pwm_config *cfg, uint16_t wrap);
void pwm_config_set_phase_correct(pwm_config *cfg, bool enabled);
void pwm_config_set_output_polarity(pwm_config *cfg, bool a, bool b);
void pwm_init(uint slice, const pwm_config *cfg, bool start);
void pwm_set_chan_level(uint slice, uint channel, uint16_t level);
void pwm_set_enabled(uint slice, bool enabled);
void pwm_set_clkdiv_int_frac(uint slice, uint8_t integer, uint8_t fraction);
void pwm_set_wrap(uint slice, uint16_t wrap);
