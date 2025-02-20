/*
 * Copyright (c) 2013-2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "cpu-boost: " fmt

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/notifier.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <linux/jiffies.h>
#include <linux/smpboot.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/time.h>
#ifdef CONFIG_LCD_NOTIFY
#include <linux/lcd_notify.h>
#endif

struct cpu_sync {
	struct delayed_work boost_rem;
	struct delayed_work input_boost_rem;
	int cpu;
	spinlock_t lock;
	bool pending;
	int src_cpu;
	unsigned int boost_min;
	unsigned int input_boost_min;
	unsigned int task_load;
};

static DEFINE_PER_CPU(struct cpu_sync, sync_info);
static DEFINE_PER_CPU(struct task_struct *, thread);
static struct workqueue_struct *cpu_boost_wq;

static struct work_struct input_boost_work;

#ifdef CONFIG_LCD_NOTIFY
static struct notifier_block notif;
#endif
static unsigned int cpu_boost = 0;
module_param(cpu_boost, uint, 0644);

static unsigned int boost_ms;
module_param(boost_ms, uint, 0644);

static unsigned int sync_threshold;
module_param(sync_threshold, uint, 0644);

static unsigned int input_boost_freq;
module_param(input_boost_freq, uint, 0644);

static unsigned int input_boost_ms = 40;
module_param(input_boost_ms, uint, 0644);

static unsigned int migration_load_threshold = 30;
module_param(migration_load_threshold, uint, 0644);

static bool load_based_syncs = 1;
module_param(load_based_syncs, bool, 0644);

static bool hotplug_boost = 1;
module_param(hotplug_boost, bool, 0644);

#ifdef CONFIG_LCD_NOTIFY
bool wakeup_boost;
module_param(wakeup_boost, bool, 0644);
#endif

static u64 last_input_time;

static unsigned int min_input_interval = 150;
module_param(min_input_interval, uint, 0644);


/*
 * The CPUFREQ_ADJUST notifier is used to override the current policy min to
 * make sure policy min >= boost_min. The cpufreq framework then does the job
 * of enforcing the new policy.
 */
static int boost_adjust_notify(struct notifier_block *nb, unsigned long val,
				void *data)
{
	struct cpufreq_policy *policy = data;
	unsigned int cpu = policy->cpu;
	struct cpu_sync *s = &per_cpu(sync_info, cpu);
	unsigned int b_min = s->boost_min;
	unsigned int ib_min = s->input_boost_min;
	unsigned int min;

	if (val != CPUFREQ_ADJUST)
		return NOTIFY_OK;

	if (!b_min && !ib_min)
		return NOTIFY_OK;

	min = max(b_min, ib_min);
	min = min(min, policy->max);

	pr_debug("CPU%u policy min before boost: %u kHz\n",
		 cpu, policy->min);
	pr_debug("CPU%u boost min: %u kHz\n", cpu, min);

	cpufreq_verify_within_limits(policy, min, UINT_MAX);

	pr_debug("CPU%u policy min after boost: %u kHz\n",
		 cpu, policy->min);

	return NOTIFY_OK;
}

static struct notifier_block boost_adjust_nb = {
	.notifier_call = boost_adjust_notify,
};

static void do_boost_rem(struct work_struct *work)
{
	struct cpu_sync *s = container_of(work, struct cpu_sync,
						boost_rem.work);

	pr_debug("Removing boost for CPU%d\n", s->cpu);
	s->boost_min = 0;
	/* Force policy re-evaluation to trigger adjust notifier. */
	cpufreq_update_policy(s->cpu);
}

static void do_input_boost_rem(struct work_struct *work)
{
	struct cpu_sync *s = container_of(work, struct cpu_sync,
						input_boost_rem.work);

	pr_debug("Removing input/hotplug boost for CPU%d\n", s->cpu);
	s->input_boost_min = 0;
	/* Force policy re-evaluation to trigger adjust notifier. */
	cpufreq_update_policy(s->cpu);
}

static int boost_migration_should_run(unsigned int cpu)
{
	struct cpu_sync *s = &per_cpu(sync_info, cpu);
	return s->pending;
}

static void run_boost_migration(unsigned int cpu)
{
	int dest_cpu = cpu;
	int src_cpu, ret;
	struct cpu_sync *s = &per_cpu(sync_info, dest_cpu);
	struct cpufreq_policy dest_policy;
	struct cpufreq_policy src_policy;
	unsigned long flags;
	unsigned int req_freq;

	spin_lock_irqsave(&s->lock, flags);
	s->pending = false;
	src_cpu = s->src_cpu;
	spin_unlock_irqrestore(&s->lock, flags);

	ret = cpufreq_get_policy(&src_policy, src_cpu);
	if (ret)
		return;

	ret = cpufreq_get_policy(&dest_policy, dest_cpu);
	if (ret)
		return;

	req_freq = load_based_syncs ?
		(dest_policy.max * s->task_load) / 100 :
						src_policy.cur;

	if (req_freq <= dest_policy.cpuinfo.min_freq) {
			pr_debug("No sync. Sync Freq:%u\n", req_freq);
		return;
	}

	if (sync_threshold)
		req_freq = min(sync_threshold, req_freq);

	cancel_delayed_work_sync(&s->boost_rem);

	s->boost_min = req_freq;

	/* Force policy re-evaluation to trigger adjust notifier. */
	get_online_cpus();
	if (cpu_online(src_cpu))
		/*
		 * Send an unchanged policy update to the source
		 * CPU. Even though the policy isn't changed from
		 * its existing boosted or non-boosted state
		 * notifying the source CPU will let the governor
		 * know a boost happened on another CPU and that it
		 * should re-evaluate the frequency at the next timer
		 * event without interference from a min sample time.
		 */
		cpufreq_update_policy(src_cpu);
	if (cpu_online(dest_cpu)) {
		cpufreq_update_policy(dest_cpu);
		queue_delayed_work_on(dest_cpu, cpu_boost_wq,
			&s->boost_rem, msecs_to_jiffies(boost_ms));
	} else {
		s->boost_min = 0;
	}
	put_online_cpus();
}

static void cpuboost_set_prio(unsigned int policy, unsigned int prio)
{
	struct sched_param param = { .sched_priority = prio };

	sched_setscheduler(current, policy, &param);
}

static void cpuboost_park(unsigned int cpu)
{
	cpuboost_set_prio(SCHED_NORMAL, 0);
}

static void cpuboost_unpark(unsigned int cpu)
{
	cpuboost_set_prio(SCHED_FIFO, MAX_RT_PRIO - 1);
}

static struct smp_hotplug_thread cpuboost_threads = {
	.store		= &thread,
	.thread_should_run = boost_migration_should_run,
	.thread_fn	= run_boost_migration,
	.thread_comm	= "boost_sync/%u",
	.park		= cpuboost_park,
	.unpark		= cpuboost_unpark,
};

static int boost_migration_notify(struct notifier_block *nb,
				unsigned long unused, void *arg)
{
	struct migration_notify_data *mnd = arg;
	unsigned long flags;
	struct cpu_sync *s = &per_cpu(sync_info, mnd->dest_cpu);

	if (load_based_syncs && (mnd->load <= migration_load_threshold))
		return NOTIFY_OK;

	if (load_based_syncs && ((mnd->load < 0) || (mnd->load > 100))) {
		pr_err("cpu-boost:Invalid load: %d\n", mnd->load);
		return NOTIFY_OK;
	}

	if (!load_based_syncs && (mnd->src_cpu == mnd->dest_cpu))
		return NOTIFY_OK;

	if (!boost_ms)
		return NOTIFY_OK;

	/* Avoid deadlock in try_to_wake_up() */
	if (thread == current)
		return NOTIFY_OK;

	pr_debug("Migration: CPU%d --> CPU%d\n", mnd->src_cpu, mnd->dest_cpu);
	spin_lock_irqsave(&s->lock, flags);
	s->pending = true;
	s->src_cpu = mnd->src_cpu;
	s->task_load = load_based_syncs ? mnd->load : 0;
	spin_unlock_irqrestore(&s->lock, flags);

	return NOTIFY_OK;
}

static struct notifier_block boost_migration_nb = {
	.notifier_call = boost_migration_notify,
};

static void do_input_boost(struct work_struct *work)
{
	unsigned int i, ret;
	struct cpu_sync *i_sync_info;
	struct cpufreq_policy policy;

	get_online_cpus();
	for_each_online_cpu(i) {

		i_sync_info = &per_cpu(sync_info, i);
		ret = cpufreq_get_policy(&policy, i);
		if (ret)
			continue;
		if (policy.cur >= input_boost_freq)
			continue;

		cancel_delayed_work_sync(&i_sync_info->input_boost_rem);
		i_sync_info->input_boost_min = input_boost_freq;
		cpufreq_update_policy(i);
		queue_delayed_work_on(i_sync_info->cpu, cpu_boost_wq,
			&i_sync_info->input_boost_rem,
			msecs_to_jiffies(input_boost_ms));
	}
	put_online_cpus();
}

static void cpuboost_input_event(struct input_handle *handle,
		unsigned int type, unsigned int code, int value)
{
	u64 now;
	unsigned int min_interval;

	if (!cpu_boost)
		return;
	if (!input_boost_freq || work_pending(&input_boost_work))

	now = ktime_to_us(ktime_get());
	min_interval = max(min_input_interval, input_boost_ms);

	if (now - last_input_time < min_interval * USEC_PER_MSEC)
		return;

	pr_debug("Input boost for input event.\n");
	queue_work(cpu_boost_wq, &input_boost_work);
	last_input_time = ktime_to_us(ktime_get());
}

bool check_cpuboost(int cpu)
{
	struct cpu_sync *i_sync_info;
	i_sync_info = &per_cpu(sync_info, cpu);

	if (i_sync_info->input_boost_min > 0)
		return true;
	return false;
}

static int cpuboost_input_connect(struct input_handler *handler,
		struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "cpufreq";

	error = input_register_handle(handle);
	if (error)
		goto err2;

	error = input_open_device(handle);
	if (error)
		goto err1;

	return 0;
err1:
	input_unregister_handle(handle);
err2:
	kfree(handle);
	return error;
}

static void cpuboost_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id cpuboost_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) },
	}, /* multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) },
	}, /* touchpad */
	{ },
};

static struct input_handler cpuboost_input_handler = {
	.event          = cpuboost_input_event,
	.connect        = cpuboost_input_connect,
	.disconnect     = cpuboost_input_disconnect,
	.name           = "cpu-boost",
	.id_table       = cpuboost_ids,
};

static int cpuboost_cpu_callback(struct notifier_block *cpu_nb,
				 unsigned long action, void *hcpu)
{
	switch (action & ~CPU_TASKS_FROZEN) {
	case CPU_UP_PREPARE:
	case CPU_DEAD:
	case CPU_UP_CANCELED:
		break;
	case CPU_ONLINE:
		if (!hotplug_boost || !input_boost_freq ||
		     work_pending(&input_boost_work))
			break;
		pr_debug("Hotplug boost for CPU%d\n", (int)hcpu);
		queue_work(cpu_boost_wq, &input_boost_work);
		last_input_time = ktime_to_us(ktime_get());
		break;
	default:
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block __refdata cpu_nblk = {
        .notifier_call = cpuboost_cpu_callback,
};

#ifdef CONFIG_LCD_NOTIFY
static int lcd_notifier_callback(struct notifier_block *this,
				unsigned long event, void *data)
{
	switch (event) {
	case LCD_EVENT_ON_START:
	case LCD_EVENT_OFF_END:
	case LCD_EVENT_OFF_START:
		break;
	case LCD_EVENT_ON_END:
		if (!wakeup_boost || !input_boost_freq ||
		     work_pending(&input_boost_work))
			break;
		pr_debug("Wakeup boost for LCD on event.\n");
		queue_work(cpu_boost_wq, &input_boost_work);
		last_input_time = ktime_to_us(ktime_get());
		break;
	default:
		break;
	}

	return NOTIFY_OK;
}
#endif

static int cpu_boost_init(void)
{
	int cpu, ret;
	struct cpu_sync *s;

	cpu_boost_wq = alloc_workqueue("cpuboost_wq", WQ_HIGHPRI, 0);
	if (!cpu_boost_wq)
		return -EFAULT;

	INIT_WORK(&input_boost_work, do_input_boost);

	for_each_possible_cpu(cpu) {
		s = &per_cpu(sync_info, cpu);
		s->cpu = cpu;
		spin_lock_init(&s->lock);
		INIT_DELAYED_WORK(&s->boost_rem, do_boost_rem);
		INIT_DELAYED_WORK(&s->input_boost_rem, do_input_boost_rem);
	}
	cpufreq_register_notifier(&boost_adjust_nb, CPUFREQ_POLICY_NOTIFIER);
	atomic_notifier_chain_register(&migration_notifier_head,
					&boost_migration_nb);

	ret = smpboot_register_percpu_thread(&cpuboost_threads);
	if (ret)
		pr_err("Cannot register cpuboost threads.\n");

	ret = input_register_handler(&cpuboost_input_handler);
	if (ret)
		pr_err("Cannot register cpuboost input handler.\n");

	ret = register_hotcpu_notifier(&cpu_nblk);
	if (ret)
		pr_err("Cannot register cpuboost hotplug handler.\n");

#ifdef CONFIG_LCD_NOTIFY
	notif.notifier_call = lcd_notifier_callback;
	ret = lcd_register_client(&notif);
        if (ret != 0)
                pr_err("Failed to register hotplug LCD notifier callback.\n");
#endif

	return ret;
}
late_initcall(cpu_boost_init);
