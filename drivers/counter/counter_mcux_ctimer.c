/*
 * Copyright (c) 2021, Toby Firth.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define DT_DRV_COMPAT nxp_lpc_ctimer

#include <drivers/counter.h>
#include <fsl_ctimer.h>
#include <logging/log.h>
#include <drivers/clock_control.h>
#include <dt-bindings/clock/mcux_lpc_syscon_clock.h>
LOG_MODULE_REGISTER(mcux_ctimer, CONFIG_COUNTER_LOG_LEVEL);

#define NUM_CHANNELS 4

struct mcux_lpc_ctimer_channel_data {
	counter_alarm_callback_t alarm_callback;
	void *alarm_user_data;
};

struct mcux_lpc_ctimer_data {
	struct mcux_lpc_ctimer_channel_data channels[NUM_CHANNELS];
};

struct mcux_lpc_ctimer_config {
	struct counter_config_info info;
	CTIMER_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	ctimer_timer_mode_t mode;
	ctimer_capture_channel_t input;
	uint32_t prescale;
	void (*irq_config_func)(const struct device *dev);
};

static int mcux_lpc_ctimer_start(const struct device *dev)
{
	const struct mcux_lpc_ctimer_config *config = dev->config;

	CTIMER_StartTimer(config->base);

	return 0;
}

static int mcux_lpc_ctimer_stop(const struct device *dev)
{
	const struct mcux_lpc_ctimer_config *config = dev->config;

	CTIMER_StopTimer(config->base);

	return 0;
}

static uint32_t mcux_lpc_ctimer_read(CTIMER_Type *base)
{
	return CTIMER_GetTimerCountValue(base);
}

static int mcux_lpc_ctimer_get_value(const struct device *dev, uint32_t *ticks)
{
	const struct mcux_lpc_ctimer_config *config = dev->config;
	*ticks = mcux_lpc_ctimer_read(config->base);
	return 0;
}

static int mcux_lpc_ctimer_set_alarm(const struct device *dev, uint8_t chan_id,
				     const struct counter_alarm_cfg *alarm_cfg)
{
	const struct mcux_lpc_ctimer_config *config = dev->config;
	struct mcux_lpc_ctimer_data *data = dev->data;

	uint32_t ticks = alarm_cfg->ticks;
	uint32_t current = mcux_lpc_ctimer_read(config->base);

	if (data->channels[chan_id].alarm_callback != NULL) {
		LOG_ERR("channel already in use");
		return -EBUSY;
	}

	if ((alarm_cfg->flags & COUNTER_ALARM_CFG_ABSOLUTE) == 0) {
		ticks += current;
	}

	data->channels[chan_id].alarm_callback = alarm_cfg->callback;
	data->channels[chan_id].alarm_user_data = alarm_cfg->user_data;

	ctimer_match_config_t match_config = { .matchValue = ticks,
					       .enableCounterReset = false,
					       .enableCounterStop = false,
					       .outControl = kCTIMER_Output_NoAction,
					       .outPinInitState = false,
					       .enableInterrupt = true };

	CTIMER_SetupMatch(config->base, chan_id, &match_config);

	return 0;
}

static int mcux_lpc_ctimer_cancel_alarm(const struct device *dev, uint8_t chan_id)
{
	const struct mcux_lpc_ctimer_config *config = dev->config;
	struct mcux_lpc_ctimer_data *data = dev->data;

	CTIMER_DisableInterrupts(config->base, (1 << chan_id));

	data->channels[chan_id].alarm_callback = NULL;
	data->channels[chan_id].alarm_user_data = NULL;

	return 0;
}

static int mcux_lpc_ctimer_set_top_value(const struct device *dev,
					 const struct counter_top_cfg *cfg)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cfg);
	return -ENOTSUP;
}

static uint32_t mcux_lpc_ctimer_get_pending_int(const struct device *dev)
{
	const struct mcux_lpc_ctimer_config *config = dev->config;

	return (CTIMER_GetStatusFlags(config->base) & 0xF) != 0;
}

static uint32_t mcux_lpc_ctimer_get_top_value(const struct device *dev)
{
	const struct mcux_lpc_ctimer_config *config = dev->config;

	return config->info.max_top_value;
}

static void mcux_lpc_ctimer_isr(const struct device *dev)
{
	const struct mcux_lpc_ctimer_config *config = dev->config;
	struct mcux_lpc_ctimer_data *data = dev->data;

	uint32_t interrupt_stat = CTIMER_GetStatusFlags(config->base);

	CTIMER_ClearStatusFlags(config->base, interrupt_stat);

	uint32_t ticks = mcux_lpc_ctimer_read(config->base);

	for (uint8_t chan = 0; chan < NUM_CHANNELS; chan++) {
		uint8_t channel_mask = 0x01 << chan;

		if (((interrupt_stat & channel_mask) != 0) &&
		    (data->channels[chan].alarm_callback != NULL)) {
			counter_alarm_callback_t alarm_callback =
				data->channels[chan].alarm_callback;
			void *alarm_user_data = data->channels[chan].alarm_user_data;

			data->channels[chan].alarm_callback = NULL;
			data->channels[chan].alarm_user_data = NULL;
			alarm_callback(dev, chan, ticks, alarm_user_data);
		}
	}
}

static int mcux_lpc_ctimer_init(const struct device *dev)
{
	/*
	 * The frequency of the timer is not known at compile time so we need to
	 * modify the timer's config in the init function at runtime when the
	 * frequency is known.
	 */
	struct mcux_lpc_ctimer_config *config = (struct mcux_lpc_ctimer_config *)dev->config;
	struct mcux_lpc_ctimer_data *data = dev->data;

	ctimer_config_t ctimer_config;

	uint32_t clk_freq = 0;

	if (clock_control_get_rate(config->clock_dev, config->clock_subsys,
					&clk_freq)) {
		LOG_ERR("unable to get clock frequency");
		return -EINVAL;
	}

	/* prescale increments when the prescale counter is 0 so if prescale is 1
	 * the counter is incremented every 2 cycles of the clock so will actually
	 * divide by 2 hence the addition of 1 to the value here.
	 */
	uint32_t freq = clk_freq / (config->prescale + 1);

	config->info.freq = freq;

	for (uint8_t chan = 0; chan < NUM_CHANNELS; chan++) {
		data->channels[chan].alarm_callback = NULL;
		data->channels[chan].alarm_user_data = NULL;
	}

	CTIMER_GetDefaultConfig(&ctimer_config);

	ctimer_config.mode = config->mode;
	ctimer_config.input = config->input;
	ctimer_config.prescale = config->prescale;

	CTIMER_Init(config->base, &ctimer_config);

	config->irq_config_func(dev);

	return 0;
}

static const struct counter_driver_api mcux_ctimer_driver_api = {
	.start = mcux_lpc_ctimer_start,
	.stop = mcux_lpc_ctimer_stop,
	.get_value = mcux_lpc_ctimer_get_value,
	.set_alarm = mcux_lpc_ctimer_set_alarm,
	.cancel_alarm = mcux_lpc_ctimer_cancel_alarm,
	.set_top_value = mcux_lpc_ctimer_set_top_value,
	.get_pending_int = mcux_lpc_ctimer_get_pending_int,
	.get_top_value = mcux_lpc_ctimer_get_top_value,
};

#define CTIMER_CLOCK_SOURCE(id) TO_CTIMER_CLOCK_SOURCE(id, DT_INST_PROP(id, clk_source))
#define TO_CTIMER_CLOCK_SOURCE(id, val) MUX_A(CM_CTIMERCLKSEL##id, val)

#define COUNTER_LPC_CTIMER_DEVICE(id)                                                              \
	static void mcux_lpc_ctimer_irq_config_##id(const struct device *dev);                     \
	static struct mcux_lpc_ctimer_config mcux_lpc_ctimer_config_##id = { \
		.info = {						\
			.max_top_value = UINT32_MAX,			\
			.freq = 1,					\
			.flags = COUNTER_CONFIG_INFO_COUNT_UP,		\
			.channels = NUM_CHANNELS,					\
		},\
		.base = (CTIMER_Type *)DT_INST_REG_ADDR(id),		\
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(id)),	\
		.clock_subsys =				\
		(clock_control_subsys_t)(DT_INST_CLOCKS_CELL(id, name) + MCUX_CTIMER_CLK_OFFSET),\
		.mode = DT_INST_PROP(id, mode),						\
		.input = DT_INST_PROP(id, input),					\
		.prescale = DT_INST_PROP(id, prescale),				\
		.irq_config_func = mcux_lpc_ctimer_irq_config_##id,	\
	};                     \
	static struct mcux_lpc_ctimer_data mcux_lpc_ctimer_data_##id;                              \
	DEVICE_DT_INST_DEFINE(id, &mcux_lpc_ctimer_init, NULL, &mcux_lpc_ctimer_data_##id,         \
			      &mcux_lpc_ctimer_config_##id, POST_KERNEL,                           \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &mcux_ctimer_driver_api);        \
	static void mcux_lpc_ctimer_irq_config_##id(const struct device *dev)                      \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(id), DT_INST_IRQ(id, priority), mcux_lpc_ctimer_isr,      \
			    DEVICE_DT_INST_GET(id), 0);                                            \
		irq_enable(DT_INST_IRQN(id));                                                      \
	}

DT_INST_FOREACH_STATUS_OKAY(COUNTER_LPC_CTIMER_DEVICE)
