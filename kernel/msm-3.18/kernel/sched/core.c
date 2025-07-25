/*
 *  kernel/sched/core.c
 *
 *  Kernel scheduler and related syscalls
 *
 *  Copyright (C) 1991-2002  Linus Torvalds
 *
 *  1996-12-23  Modified by Dave Grothe to fix bugs in semaphores and
 *		make semaphores SMP safe
 *  1998-11-19	Implemented schedule_timeout() and related stuff
 *		by Andrea Arcangeli
 *  2002-01-04	New ultra-scalable O(1) scheduler by Ingo Molnar:
 *		hybrid priority-list and round-robin design with
 *		an array-switch method of distributing timeslices
 *		and per-CPU runqueues.  Cleanups and useful suggestions
 *		by Davide Libenzi, preemptible kernel bits by Robert Love.
 *  2003-09-03	Interactivity tuning by Con Kolivas.
 *  2004-04-02	Scheduler domains code by Nick Piggin
 *  2007-04-15  Work begun on replacing all interactivity tuning with a
 *              fair scheduling design by Con Kolivas.
 *  2007-05-05  Load balancing (smp-nice) and other improvements
 *              by Peter Williams
 *  2007-05-06  Interactivity improvements to CFS by Mike Galbraith
 *  2007-07-01  Group scheduling enhancements by Srivatsa Vaddagiri
 *  2007-11-29  RT balancing improvements by Steven Rostedt, Gregory Haskins,
 *              Thomas Gleixner, Mike Kravetz
 */

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/nmi.h>
#include <linux/init.h>
#include <linux/uaccess.h>
#include <linux/highmem.h>
#include <asm/mmu_context.h>
#include <linux/interrupt.h>
#include <linux/capability.h>
#include <linux/completion.h>
#include <linux/kernel_stat.h>
#include <linux/debug_locks.h>
#include <linux/perf_event.h>
#include <linux/security.h>
#include <linux/notifier.h>
#include <linux/profile.h>
#include <linux/freezer.h>
#include <linux/vmalloc.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/pid_namespace.h>
#include <linux/smp.h>
#include <linux/threads.h>
#include <linux/timer.h>
#include <linux/rcupdate.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/percpu.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sysctl.h>
#include <linux/syscalls.h>
#include <linux/times.h>
#include <linux/tsacct_kern.h>
#include <linux/kprobes.h>
#include <linux/delayacct.h>
#include <linux/unistd.h>
#include <linux/pagemap.h>
#include <linux/hrtimer.h>
#include <linux/tick.h>
#include <linux/debugfs.h>
#include <linux/ctype.h>
#include <linux/ftrace.h>
#include <linux/slab.h>
#include <linux/init_task.h>
#include <linux/binfmts.h>
#include <linux/context_tracking.h>
#include <linux/compiler.h>
#include <linux/cpufreq.h>
#include <linux/syscore_ops.h>
#include <linux/list_sort.h>

#include <asm/switch_to.h>
#include <asm/tlb.h>
#include <asm/irq_regs.h>
#include <asm/mutex.h>
#ifdef CONFIG_PARAVIRT
#include <asm/paravirt.h>
#endif
#ifdef CONFIG_MSM_APP_SETTINGS
#include <asm/app_api.h>
#endif

#include "sched.h"
#include "../workqueue_internal.h"
#include "../smpboot.h"

#define CREATE_TRACE_POINTS
#include <trace/events/sched.h>

const char *task_event_names[] = {"PUT_PREV_TASK", "PICK_NEXT_TASK",
				  "TASK_WAKE", "TASK_MIGRATE", "TASK_UPDATE",
				"IRQ_UPDATE"};

const char *migrate_type_names[] = {"GROUP_TO_RQ", "RQ_TO_GROUP",
					 "RQ_TO_RQ", "GROUP_TO_GROUP"};

ATOMIC_NOTIFIER_HEAD(migration_notifier_head);
ATOMIC_NOTIFIER_HEAD(load_alert_notifier_head);

void start_bandwidth_timer(struct hrtimer *period_timer, ktime_t period)
{
	unsigned long delta;
	ktime_t soft, hard, now;

	for (;;) {
		if (hrtimer_active(period_timer))
			break;

		now = hrtimer_cb_get_time(period_timer);
		hrtimer_forward(period_timer, now, period);

		soft = hrtimer_get_softexpires(period_timer);
		hard = hrtimer_get_expires(period_timer);
		delta = ktime_to_ns(ktime_sub(hard, soft));
		__hrtimer_start_range_ns(period_timer, soft, delta,
					 HRTIMER_MODE_ABS_PINNED, 0);
	}
}

DEFINE_MUTEX(sched_domains_mutex);
DEFINE_PER_CPU_SHARED_ALIGNED(struct rq, runqueues);

static void update_rq_clock_task(struct rq *rq, s64 delta);

void update_rq_clock(struct rq *rq)
{
	s64 delta;

	if (rq->skip_clock_update > 0)
		return;

	delta = sched_clock_cpu(cpu_of(rq)) - rq->clock;
	if (delta < 0)
		return;
	rq->clock += delta;
	update_rq_clock_task(rq, delta);
}

/*
 * Debugging: various feature bits
 */

#define SCHED_FEAT(name, enabled)	\
	(1UL << __SCHED_FEAT_##name) * enabled |

const_debug unsigned int sysctl_sched_features =
#include "features.h"
	0;

#undef SCHED_FEAT

#ifdef CONFIG_SCHED_DEBUG
#define SCHED_FEAT(name, enabled)	\
	#name ,

static const char * const sched_feat_names[] = {
#include "features.h"
};

#undef SCHED_FEAT

static int sched_feat_show(struct seq_file *m, void *v)
{
	int i;

	for (i = 0; i < __SCHED_FEAT_NR; i++) {
		if (!(sysctl_sched_features & (1UL << i)))
			seq_puts(m, "NO_");
		seq_printf(m, "%s ", sched_feat_names[i]);
	}
	seq_puts(m, "\n");

	return 0;
}

#ifdef HAVE_JUMP_LABEL

#define jump_label_key__true  STATIC_KEY_INIT_TRUE
#define jump_label_key__false STATIC_KEY_INIT_FALSE

#define SCHED_FEAT(name, enabled)	\
	jump_label_key__##enabled ,

struct static_key sched_feat_keys[__SCHED_FEAT_NR] = {
#include "features.h"
};

#undef SCHED_FEAT

static void sched_feat_disable(int i)
{
	if (static_key_enabled(&sched_feat_keys[i]))
		static_key_slow_dec(&sched_feat_keys[i]);
}

static void sched_feat_enable(int i)
{
	if (!static_key_enabled(&sched_feat_keys[i]))
		static_key_slow_inc(&sched_feat_keys[i]);
}
#else
static void sched_feat_disable(int i) { };
static void sched_feat_enable(int i) { };
#endif /* HAVE_JUMP_LABEL */

static int sched_feat_set(char *cmp)
{
	int i;
	int neg = 0;

	if (strncmp(cmp, "NO_", 3) == 0) {
		neg = 1;
		cmp += 3;
	}

	for (i = 0; i < __SCHED_FEAT_NR; i++) {
		if (strcmp(cmp, sched_feat_names[i]) == 0) {
			if (neg) {
				sysctl_sched_features &= ~(1UL << i);
				sched_feat_disable(i);
			} else {
				sysctl_sched_features |= (1UL << i);
				sched_feat_enable(i);
			}
			break;
		}
	}

	return i;
}

static ssize_t
sched_feat_write(struct file *filp, const char __user *ubuf,
		size_t cnt, loff_t *ppos)
{
	char buf[64];
	char *cmp;
	int i;
	struct inode *inode;

	if (cnt > 63)
		cnt = 63;

	if (copy_from_user(&buf, ubuf, cnt))
		return -EFAULT;

	buf[cnt] = 0;
	cmp = strstrip(buf);

	/* Ensure the static_key remains in a consistent state */
	inode = file_inode(filp);
	mutex_lock(&inode->i_mutex);
	i = sched_feat_set(cmp);
	mutex_unlock(&inode->i_mutex);
	if (i == __SCHED_FEAT_NR)
		return -EINVAL;

	*ppos += cnt;

	return cnt;
}

static int sched_feat_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, sched_feat_show, NULL);
}

static const struct file_operations sched_feat_fops = {
	.open		= sched_feat_open,
	.write		= sched_feat_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static __init int sched_init_debug(void)
{
	debugfs_create_file("sched_features", 0644, NULL, NULL,
			&sched_feat_fops);

	return 0;
}
late_initcall(sched_init_debug);
#endif /* CONFIG_SCHED_DEBUG */

/*
 * Number of tasks to iterate in a single balance run.
 * Limited because this is done with IRQs disabled.
 */
const_debug unsigned int sysctl_sched_nr_migrate = 32;

/*
 * period over which we average the RT time consumption, measured
 * in ms.
 *
 * default: 1s
 */
const_debug unsigned int sysctl_sched_time_avg = MSEC_PER_SEC;

/*
 * period over which we measure -rt task cpu usage in us.
 * default: 1s
 */
unsigned int sysctl_sched_rt_period = 1000000;

__read_mostly int scheduler_running;

/*
 * part of the period that we allow rt tasks to run in us.
 * default: 0.95s
 */
int sysctl_sched_rt_runtime = 950000;

/*
 * __task_rq_lock - lock the rq @p resides on.
 */
static inline struct rq *__task_rq_lock(struct task_struct *p)
	__acquires(rq->lock)
{
	struct rq *rq;

	lockdep_assert_held(&p->pi_lock);

	for (;;) {
		rq = task_rq(p);
		raw_spin_lock(&rq->lock);
		if (likely(rq == task_rq(p) && !task_on_rq_migrating(p)))
			return rq;
		raw_spin_unlock(&rq->lock);

		while (unlikely(task_on_rq_migrating(p)))
			cpu_relax();
	}
}

/*
 * task_rq_lock - lock p->pi_lock and lock the rq @p resides on.
 */
static struct rq *task_rq_lock(struct task_struct *p, unsigned long *flags)
	__acquires(p->pi_lock)
	__acquires(rq->lock)
{
	struct rq *rq;

	for (;;) {
		raw_spin_lock_irqsave(&p->pi_lock, *flags);
		rq = task_rq(p);
		raw_spin_lock(&rq->lock);
		if (likely(rq == task_rq(p) && !task_on_rq_migrating(p)))
			return rq;
		raw_spin_unlock(&rq->lock);
		raw_spin_unlock_irqrestore(&p->pi_lock, *flags);

		while (unlikely(task_on_rq_migrating(p)))
			cpu_relax();
	}
}

static void __task_rq_unlock(struct rq *rq)
	__releases(rq->lock)
{
	raw_spin_unlock(&rq->lock);
}

static inline void
task_rq_unlock(struct rq *rq, struct task_struct *p, unsigned long *flags)
	__releases(rq->lock)
	__releases(p->pi_lock)
{
	raw_spin_unlock(&rq->lock);
	raw_spin_unlock_irqrestore(&p->pi_lock, *flags);
}

/*
 * this_rq_lock - lock this runqueue and disable interrupts.
 */
static struct rq *this_rq_lock(void)
	__acquires(rq->lock)
{
	struct rq *rq;

	local_irq_disable();
	rq = this_rq();
	raw_spin_lock(&rq->lock);

	return rq;
}

#ifdef CONFIG_SCHED_HRTICK
/*
 * Use HR-timers to deliver accurate preemption points.
 */

static void hrtick_clear(struct rq *rq)
{
	if (hrtimer_active(&rq->hrtick_timer))
		hrtimer_cancel(&rq->hrtick_timer);
}

/*
 * High-resolution timer tick.
 * Runs from hardirq context with interrupts disabled.
 */
static enum hrtimer_restart hrtick(struct hrtimer *timer)
{
	struct rq *rq = container_of(timer, struct rq, hrtick_timer);

	WARN_ON_ONCE(cpu_of(rq) != smp_processor_id());

	raw_spin_lock(&rq->lock);
	update_rq_clock(rq);
	rq->curr->sched_class->task_tick(rq, rq->curr, 1);
	raw_spin_unlock(&rq->lock);

	return HRTIMER_NORESTART;
}

#ifdef CONFIG_SMP

static int __hrtick_restart(struct rq *rq)
{
	struct hrtimer *timer = &rq->hrtick_timer;
	ktime_t time = hrtimer_get_softexpires(timer);

	return __hrtimer_start_range_ns(timer, time, 0, HRTIMER_MODE_ABS_PINNED, 0);
}

/*
 * called from hardirq (IPI) context
 */
static void __hrtick_start(void *arg)
{
	struct rq *rq = arg;

	raw_spin_lock(&rq->lock);
	__hrtick_restart(rq);
	rq->hrtick_csd_pending = 0;
	raw_spin_unlock(&rq->lock);
}

/*
 * Called to set the hrtick timer state.
 *
 * called with rq->lock held and irqs disabled
 */
void hrtick_start(struct rq *rq, u64 delay)
{
	struct hrtimer *timer = &rq->hrtick_timer;
	ktime_t time;
	s64 delta;

	/*
	 * Don't schedule slices shorter than 10000ns, that just
	 * doesn't make sense and can cause timer DoS.
	 */
	delta = max_t(s64, delay, 10000LL);
	time = ktime_add_ns(timer->base->get_time(), delta);

	hrtimer_set_expires(timer, time);

	if (rq == this_rq()) {
		__hrtick_restart(rq);
	} else if (!rq->hrtick_csd_pending) {
		smp_call_function_single_async(cpu_of(rq), &rq->hrtick_csd);
		rq->hrtick_csd_pending = 1;
	}
}

static int
hotplug_hrtick(struct notifier_block *nfb, unsigned long action, void *hcpu)
{
	int cpu = (int)(long)hcpu;

	switch (action) {
	case CPU_UP_CANCELED:
	case CPU_UP_CANCELED_FROZEN:
	case CPU_DOWN_PREPARE:
	case CPU_DOWN_PREPARE_FROZEN:
	case CPU_DEAD:
	case CPU_DEAD_FROZEN:
		hrtick_clear(cpu_rq(cpu));
		return NOTIFY_OK;
	}

	return NOTIFY_DONE;
}

static __init void init_hrtick(void)
{
	hotcpu_notifier(hotplug_hrtick, 0);
}
#else
/*
 * Called to set the hrtick timer state.
 *
 * called with rq->lock held and irqs disabled
 */
void hrtick_start(struct rq *rq, u64 delay)
{
	/*
	 * Don't schedule slices shorter than 10000ns, that just
	 * doesn't make sense. Rely on vruntime for fairness.
	 */
	delay = max_t(u64, delay, 10000LL);
	__hrtimer_start_range_ns(&rq->hrtick_timer, ns_to_ktime(delay), 0,
			HRTIMER_MODE_REL_PINNED, 0);
}

static inline void init_hrtick(void)
{
}
#endif /* CONFIG_SMP */

static void init_rq_hrtick(struct rq *rq)
{
#ifdef CONFIG_SMP
	rq->hrtick_csd_pending = 0;

	rq->hrtick_csd.flags = 0;
	rq->hrtick_csd.func = __hrtick_start;
	rq->hrtick_csd.info = rq;
#endif

	hrtimer_init(&rq->hrtick_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	rq->hrtick_timer.function = hrtick;
}
#else	/* CONFIG_SCHED_HRTICK */
static inline void hrtick_clear(struct rq *rq)
{
}

static inline void init_rq_hrtick(struct rq *rq)
{
}

static inline void init_hrtick(void)
{
}
#endif	/* CONFIG_SCHED_HRTICK */

/*
 * cmpxchg based fetch_or, macro so it works for different integer types
 */
#define fetch_or(ptr, val)						\
({	typeof(*(ptr)) __old, __val = *(ptr);				\
 	for (;;) {							\
 		__old = cmpxchg((ptr), __val, __val | (val));		\
 		if (__old == __val)					\
 			break;						\
 		__val = __old;						\
 	}								\
 	__old;								\
})

#if defined(CONFIG_SMP) && defined(TIF_POLLING_NRFLAG)
/*
 * Atomically set TIF_NEED_RESCHED and test for TIF_POLLING_NRFLAG,
 * this avoids any races wrt polling state changes and thereby avoids
 * spurious IPIs.
 */
static bool set_nr_and_not_polling(struct task_struct *p)
{
	struct thread_info *ti = task_thread_info(p);
	return !(fetch_or(&ti->flags, _TIF_NEED_RESCHED) & _TIF_POLLING_NRFLAG);
}

/*
 * Atomically set TIF_NEED_RESCHED if TIF_POLLING_NRFLAG is set.
 *
 * If this returns true, then the idle task promises to call
 * sched_ttwu_pending() and reschedule soon.
 */
static bool set_nr_if_polling(struct task_struct *p)
{
	struct thread_info *ti = task_thread_info(p);
	typeof(ti->flags) old, val = ACCESS_ONCE(ti->flags);

	for (;;) {
		if (!(val & _TIF_POLLING_NRFLAG))
			return false;
		if (val & _TIF_NEED_RESCHED)
			return true;
		old = cmpxchg(&ti->flags, val, val | _TIF_NEED_RESCHED);
		if (old == val)
			break;
		val = old;
	}
	return true;
}

#else
static bool set_nr_and_not_polling(struct task_struct *p)
{
	set_tsk_need_resched(p);
	return true;
}

#ifdef CONFIG_SMP
static bool set_nr_if_polling(struct task_struct *p)
{
	return false;
}
#endif
#endif

/*
 * resched_curr - mark rq's current task 'to be rescheduled now'.
 *
 * On UP this means the setting of the need_resched flag, on SMP it
 * might also involve a cross-CPU call to trigger the scheduler on
 * the target CPU.
 */
void resched_curr(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	int cpu;

	lockdep_assert_held(&rq->lock);

	if (test_tsk_need_resched(curr))
		return;

	cpu = cpu_of(rq);

	if (cpu == smp_processor_id()) {
		set_tsk_need_resched(curr);
		set_preempt_need_resched();
		return;
	}

	if (set_nr_and_not_polling(curr))
		smp_send_reschedule(cpu);
	else
		trace_sched_wake_idle_without_ipi(cpu);
}

void resched_cpu(int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	unsigned long flags;

	raw_spin_lock_irqsave(&rq->lock, flags);
	if (cpu_online(cpu) || cpu == smp_processor_id())
		resched_curr(rq);
	raw_spin_unlock_irqrestore(&rq->lock, flags);
}

#ifdef CONFIG_SMP
#ifdef CONFIG_NO_HZ_COMMON
/*
 * In the semi idle case, use the nearest busy cpu for migrating timers
 * from an idle cpu.  This is good for power-savings.
 *
 * We don't do similar optimization for completely idle system, as
 * selecting an idle cpu will add more delays to the timers than intended
 * (as that cpu's timer base may not be uptodate wrt jiffies etc).
 */
int get_nohz_timer_target(int pinned)
{
	int cpu = smp_processor_id();
	int i;
	struct sched_domain *sd;

	if (pinned || !get_sysctl_timer_migration() || !idle_cpu(cpu))
		return cpu;

	rcu_read_lock();
	for_each_domain(cpu, sd) {
		for_each_cpu(i, sched_domain_span(sd)) {
			if (!idle_cpu(i)) {
				cpu = i;
				goto unlock;
			}
		}
	}
unlock:
	rcu_read_unlock();
	return cpu;
}
/*
 * When add_timer_on() enqueues a timer into the timer wheel of an
 * idle CPU then this timer might expire before the next timer event
 * which is scheduled to wake up that CPU. In case of a completely
 * idle system the next event might even be infinite time into the
 * future. wake_up_idle_cpu() ensures that the CPU is woken up and
 * leaves the inner idle loop so the newly added timer is taken into
 * account when the CPU goes back to idle and evaluates the timer
 * wheel for the next timer event.
 */
static void wake_up_idle_cpu(int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	if (cpu == smp_processor_id())
		return;

	if (set_nr_and_not_polling(rq->idle))
		smp_send_reschedule(cpu);
	else
		trace_sched_wake_idle_without_ipi(cpu);
}

static bool wake_up_full_nohz_cpu(int cpu)
{
	/*
	 * We just need the target to call irq_exit() and re-evaluate
	 * the next tick. The nohz full kick at least implies that.
	 * If needed we can still optimize that later with an
	 * empty IRQ.
	 */
	if (tick_nohz_full_cpu(cpu)) {
		if (cpu != smp_processor_id() ||
		    tick_nohz_tick_stopped())
			tick_nohz_full_kick_cpu(cpu);
		return true;
	}

	return false;
}

void wake_up_nohz_cpu(int cpu)
{
	if (!wake_up_full_nohz_cpu(cpu))
		wake_up_idle_cpu(cpu);
}

static inline bool got_nohz_idle_kick(void)
{
	int cpu = smp_processor_id();

	if (!test_bit(NOHZ_BALANCE_KICK, nohz_flags(cpu)))
		return false;

	if (idle_cpu(cpu) && !need_resched())
		return true;

	/*
	 * We can't run Idle Load Balance on this CPU for this time so we
	 * cancel it and clear NOHZ_BALANCE_KICK
	 */
	clear_bit(NOHZ_BALANCE_KICK, nohz_flags(cpu));
	return false;
}

#else /* CONFIG_NO_HZ_COMMON */

static inline bool got_nohz_idle_kick(void)
{
	return false;
}

#endif /* CONFIG_NO_HZ_COMMON */

#ifdef CONFIG_NO_HZ_FULL
bool sched_can_stop_tick(void)
{
	/*
	 * More than one running task need preemption.
	 * nr_running update is assumed to be visible
	 * after IPI is sent from wakers.
	 */
	if (this_rq()->nr_running > 1)
		return false;

	return true;
}
#endif /* CONFIG_NO_HZ_FULL */

void sched_avg_update(struct rq *rq)
{
	s64 period = sched_avg_period();

	while ((s64)(rq_clock(rq) - rq->age_stamp) > period) {
		/*
		 * Inline assembly required to prevent the compiler
		 * optimising this loop into a divmod call.
		 * See __iter_div_u64_rem() for another example of this.
		 */
		asm("" : "+rm" (rq->age_stamp));
		rq->age_stamp += period;
		rq->rt_avg /= 2;
	}
}

#ifdef CONFIG_SCHED_HMP

/*
 * Note C-state for (idle) cpus.
 *
 * @cstate = cstate index, 0 -> active state
 * @wakeup_energy = energy spent in waking up cpu
 * @wakeup_latency = latency to wakeup from cstate
 *
 */
void
sched_set_cpu_cstate(int cpu, int cstate, int wakeup_energy, int wakeup_latency)
{
	struct rq *rq = cpu_rq(cpu);

	rq->cstate = cstate; /* C1, C2 etc */
	rq->wakeup_energy = wakeup_energy;
	rq->wakeup_latency = wakeup_latency;
}

/*
 * Note D-state for (idle) cluster.
 *
 * @dstate = dstate index, 0 -> active state
 * @wakeup_energy = energy spent in waking up cluster
 * @wakeup_latency = latency to wakeup from cluster
 *
 */
void sched_set_cluster_dstate(const cpumask_t *cluster_cpus, int dstate,
			int wakeup_energy, int wakeup_latency)
{
	struct sched_cluster *cluster =
		cpu_rq(cpumask_first(cluster_cpus))->cluster;
	cluster->dstate = dstate;
	cluster->dstate_wakeup_energy = wakeup_energy;
	cluster->dstate_wakeup_latency = wakeup_latency;
}

#endif /* CONFIG_SCHED_HMP */

#endif /* CONFIG_SMP */

#ifdef CONFIG_SCHED_HMP

static ktime_t ktime_last;
static bool sched_ktime_suspended;

static bool use_cycle_counter;
static struct cpu_cycle_counter_cb cpu_cycle_counter_cb;

u64 sched_ktime_clock(void)
{
	if (unlikely(sched_ktime_suspended))
		return ktime_to_ns(ktime_last);
	return ktime_get_ns();
}

static void sched_resume(void)
{
	sched_ktime_suspended = false;
}

static int sched_suspend(void)
{
	ktime_last = ktime_get();
	sched_ktime_suspended = true;
	return 0;
}

static struct syscore_ops sched_syscore_ops = {
	.resume	= sched_resume,
	.suspend = sched_suspend
};

static int __init sched_init_ops(void)
{
	register_syscore_ops(&sched_syscore_ops);
	return 0;
}
late_initcall(sched_init_ops);

static inline void clear_ed_task(struct task_struct *p, struct rq *rq)
{
	if (p == rq->ed_task)
		rq->ed_task = NULL;
}

static inline void set_task_last_wake(struct task_struct *p, u64 wallclock)
{
	p->last_wake_ts = wallclock;
}

static inline void set_task_last_switch_out(struct task_struct *p,
					    u64 wallclock)
{
	p->last_switch_out_ts = wallclock;
}
#else
u64 sched_ktime_clock(void)
{
	return 0;
}

static inline void clear_ed_task(struct task_struct *p, struct rq *rq) {}
static inline void set_task_last_wake(struct task_struct *p, u64 wallclock) {}
static inline void set_task_last_switch_out(struct task_struct *p,
					    u64 wallclock) {}
#endif

#if defined(CONFIG_RT_GROUP_SCHED) || (defined(CONFIG_FAIR_GROUP_SCHED) && \
			(defined(CONFIG_SMP) || defined(CONFIG_CFS_BANDWIDTH)))
/*
 * Iterate task_group tree rooted at *from, calling @down when first entering a
 * node and @up when leaving it for the final time.
 *
 * Caller must hold rcu_lock or sufficient equivalent.
 */
int walk_tg_tree_from(struct task_group *from,
			     tg_visitor down, tg_visitor up, void *data)
{
	struct task_group *parent, *child;
	int ret;

	parent = from;

down:
	ret = (*down)(parent, data);
	if (ret)
		goto out;
	list_for_each_entry_rcu(child, &parent->children, siblings) {
		parent = child;
		goto down;

up:
		continue;
	}
	ret = (*up)(parent, data);
	if (ret || parent == from)
		goto out;

	child = parent;
	parent = parent->parent;
	if (parent)
		goto up;
out:
	return ret;
}

int tg_nop(struct task_group *tg, void *data)
{
	return 0;
}
#endif

static void set_load_weight(struct task_struct *p)
{
	int prio = p->static_prio - MAX_RT_PRIO;
	struct load_weight *load = &p->se.load;

	/*
	 * SCHED_IDLE tasks get minimal weight:
	 */
	if (p->policy == SCHED_IDLE) {
		load->weight = scale_load(WEIGHT_IDLEPRIO);
		load->inv_weight = WMULT_IDLEPRIO;
		return;
	}

	load->weight = scale_load(prio_to_weight[prio]);
	load->inv_weight = prio_to_wmult[prio];
}

static inline void enqueue_task(struct rq *rq, struct task_struct *p, int flags)
{
	update_rq_clock(rq);
	if (!(flags & ENQUEUE_RESTORE))
		sched_info_queued(rq, p);
	p->sched_class->enqueue_task(rq, p, flags);
	trace_sched_enq_deq_task(p, 1);
}

static inline void dequeue_task(struct rq *rq, struct task_struct *p, int flags)
{
	update_rq_clock(rq);
	if (!(flags & DEQUEUE_SAVE))
		sched_info_dequeued(rq, p);
	p->sched_class->dequeue_task(rq, p, flags);
	trace_sched_enq_deq_task(p, 0);
}

void activate_task(struct rq *rq, struct task_struct *p, int flags)
{
	if (task_contributes_to_load(p))
		rq->nr_uninterruptible--;

	enqueue_task(rq, p, flags);
}

void deactivate_task(struct rq *rq, struct task_struct *p, int flags)
{
	if (task_contributes_to_load(p))
		rq->nr_uninterruptible++;

	if (flags & DEQUEUE_SLEEP)
		clear_ed_task(p, rq);

	dequeue_task(rq, p, flags);
}

static void update_rq_clock_task(struct rq *rq, s64 delta)
{
/*
 * In theory, the compile should just see 0 here, and optimize out the call
 * to sched_rt_avg_update. But I don't trust it...
 */
#if defined(CONFIG_IRQ_TIME_ACCOUNTING) || defined(CONFIG_PARAVIRT_TIME_ACCOUNTING)
	s64 steal = 0, irq_delta = 0;
#endif
#ifdef CONFIG_IRQ_TIME_ACCOUNTING
	irq_delta = irq_time_read(cpu_of(rq)) - rq->prev_irq_time;

	/*
	 * Since irq_time is only updated on {soft,}irq_exit, we might run into
	 * this case when a previous update_rq_clock() happened inside a
	 * {soft,}irq region.
	 *
	 * When this happens, we stop ->clock_task and only update the
	 * prev_irq_time stamp to account for the part that fit, so that a next
	 * update will consume the rest. This ensures ->clock_task is
	 * monotonic.
	 *
	 * It does however cause some slight miss-attribution of {soft,}irq
	 * time, a more accurate solution would be to update the irq_time using
	 * the current rq->clock timestamp, except that would require using
	 * atomic ops.
	 */
	if (irq_delta > delta)
		irq_delta = delta;

	rq->prev_irq_time += irq_delta;
	delta -= irq_delta;
#endif
#ifdef CONFIG_PARAVIRT_TIME_ACCOUNTING
	if (static_key_false((&paravirt_steal_rq_enabled))) {
		steal = paravirt_steal_clock(cpu_of(rq));
		steal -= rq->prev_steal_time_rq;

		if (unlikely(steal > delta))
			steal = delta;

		rq->prev_steal_time_rq += steal;
		delta -= steal;
	}
#endif

	rq->clock_task += delta;

#if defined(CONFIG_IRQ_TIME_ACCOUNTING) || defined(CONFIG_PARAVIRT_TIME_ACCOUNTING)
	if ((irq_delta + steal) && sched_feat(NONTASK_CAPACITY))
		sched_rt_avg_update(rq, irq_delta + steal);
#endif
}

void sched_set_stop_task(int cpu, struct task_struct *stop)
{
	struct sched_param param = { .sched_priority = MAX_RT_PRIO - 1 };
	struct task_struct *old_stop = cpu_rq(cpu)->stop;

	if (stop) {
		/*
		 * Make it appear like a SCHED_FIFO task, its something
		 * userspace knows about and won't get confused about.
		 *
		 * Also, it will make PI more or less work without too
		 * much confusion -- but then, stop work should not
		 * rely on PI working anyway.
		 */
		sched_setscheduler_nocheck(stop, SCHED_FIFO, &param);

		stop->sched_class = &stop_sched_class;
	}

	cpu_rq(cpu)->stop = stop;

	if (old_stop) {
		/*
		 * Reset it back to a normal scheduling class so that
		 * it can die in pieces.
		 */
		old_stop->sched_class = &rt_sched_class;
	}
}

/*
 * __normal_prio - return the priority that is based on the static prio
 */
static inline int __normal_prio(struct task_struct *p)
{
	return p->static_prio;
}

/*
 * Calculate the expected normal priority: i.e. priority
 * without taking RT-inheritance into account. Might be
 * boosted by interactivity modifiers. Changes upon fork,
 * setprio syscalls, and whenever the interactivity
 * estimator recalculates.
 */
static inline int normal_prio(struct task_struct *p)
{
	int prio;

	if (task_has_dl_policy(p))
		prio = MAX_DL_PRIO-1;
	else if (task_has_rt_policy(p))
		prio = MAX_RT_PRIO-1 - p->rt_priority;
	else
		prio = __normal_prio(p);
	return prio;
}

/*
 * Calculate the current priority, i.e. the priority
 * taken into account by the scheduler. This value might
 * be boosted by RT tasks, or might be boosted by
 * interactivity modifiers. Will be RT if the task got
 * RT-boosted. If not then it returns p->normal_prio.
 */
static int effective_prio(struct task_struct *p)
{
	p->normal_prio = normal_prio(p);
	/*
	 * If we are RT tasks or we were boosted to RT priority,
	 * keep the priority unchanged. Otherwise, update priority
	 * to the normal priority:
	 */
	if (!rt_prio(p->prio))
		return p->normal_prio;
	return p->prio;
}

/**
 * task_curr - is this task currently executing on a CPU?
 * @p: the task in question.
 *
 * Return: 1 if the task is currently executing. 0 otherwise.
 */
inline int task_curr(const struct task_struct *p)
{
	return cpu_curr(task_cpu(p)) == p;
}

static inline void check_class_changed(struct rq *rq, struct task_struct *p,
				       const struct sched_class *prev_class,
				       int oldprio)
{
	if (prev_class != p->sched_class) {
		if (prev_class->switched_from)
			prev_class->switched_from(rq, p);
		p->sched_class->switched_to(rq, p);
	} else if (oldprio != p->prio || dl_task(p))
		p->sched_class->prio_changed(rq, p, oldprio);
}

void check_preempt_curr(struct rq *rq, struct task_struct *p, int flags)
{
	const struct sched_class *class;

	if (p->sched_class == rq->curr->sched_class) {
		rq->curr->sched_class->check_preempt_curr(rq, p, flags);
	} else {
		for_each_class(class) {
			if (class == rq->curr->sched_class)
				break;
			if (class == p->sched_class) {
				resched_curr(rq);
				break;
			}
		}
	}

	/*
	 * A queue event has occurred, and we're going to schedule.  In
	 * this case, we can save a useless back to back clock update.
	 */
	if (task_on_rq_queued(rq->curr) && test_tsk_need_resched(rq->curr))
		rq->skip_clock_update = 1;
}

#ifdef CONFIG_SCHED_HMP
unsigned int max_possible_efficiency = 1;
unsigned int min_possible_efficiency = UINT_MAX;

unsigned long __weak arch_get_cpu_efficiency(int cpu)
{
	return SCHED_LOAD_SCALE;
}

/* Keep track of max/min capacity possible across CPUs "currently" */
static void __update_min_max_capacity(void)
{
	int i;
	int max_cap = 0, min_cap = INT_MAX;

	for_each_online_cpu(i) {
		max_cap = max(max_cap, cpu_capacity(i));
		min_cap = min(min_cap, cpu_capacity(i));
	}

	max_capacity = max_cap;
	min_capacity = min_cap;
}

static void update_min_max_capacity(void)
{
	unsigned long flags;
	int i;

	local_irq_save(flags);
	for_each_possible_cpu(i)
		raw_spin_lock(&cpu_rq(i)->lock);

	__update_min_max_capacity();

	for_each_possible_cpu(i)
		raw_spin_unlock(&cpu_rq(i)->lock);
	local_irq_restore(flags);
}

/*
 * Return 'capacity' of a cpu in reference to "least" efficient cpu, such that
 * least efficient cpu gets capacity of 1024
 */
static unsigned long
capacity_scale_cpu_efficiency(struct sched_cluster *cluster)
{
	return (1024 * cluster->efficiency) / min_possible_efficiency;
}

/*
 * Return 'capacity' of a cpu in reference to cpu with lowest max_freq
 * (min_max_freq), such that one with lowest max_freq gets capacity of 1024.
 */
static unsigned long capacity_scale_cpu_freq(struct sched_cluster *cluster)
{
	return (1024 * cluster_max_freq(cluster)) / min_max_freq;
}

/*
 * Return load_scale_factor of a cpu in reference to "most" efficient cpu, so
 * that "most" efficient cpu gets a load_scale_factor of 1
 */
static inline unsigned long
load_scale_cpu_efficiency(struct sched_cluster *cluster)
{
	return DIV_ROUND_UP(1024 * max_possible_efficiency,
			    cluster->efficiency);
}

/*
 * Return load_scale_factor of a cpu in reference to cpu with best max_freq
 * (max_possible_freq), so that one with best max_freq gets a load_scale_factor
 * of 1.
 */
static inline unsigned long load_scale_cpu_freq(struct sched_cluster *cluster)
{
	return DIV_ROUND_UP(1024 * max_possible_freq,
			   cluster_max_freq(cluster));
}

static int compute_capacity(struct sched_cluster *cluster)
{
	int capacity = 1024;

	capacity *= capacity_scale_cpu_efficiency(cluster);
	capacity >>= 10;

	capacity *= capacity_scale_cpu_freq(cluster);
	capacity >>= 10;

	return capacity;
}

static int compute_max_possible_capacity(struct sched_cluster *cluster)
{
	int capacity = 1024;

	capacity *= capacity_scale_cpu_efficiency(cluster);
	capacity >>= 10;

	capacity *= (1024 * cluster->max_possible_freq) / min_max_freq;
	capacity >>= 10;

	return capacity;
}

static int compute_load_scale_factor(struct sched_cluster *cluster)
{
	int load_scale = 1024;

	/*
	 * load_scale_factor accounts for the fact that task load
	 * is in reference to "best" performing cpu. Task's load will need to be
	 * scaled (up) by a factor to determine suitability to be placed on a
	 * (little) cpu.
	 */
	load_scale *= load_scale_cpu_efficiency(cluster);
	load_scale >>= 10;

	load_scale *= load_scale_cpu_freq(cluster);
	load_scale >>= 10;

	return load_scale;
}

struct list_head cluster_head;
static DEFINE_MUTEX(cluster_lock);
static cpumask_t all_cluster_cpus = CPU_MASK_NONE;
DECLARE_BITMAP(all_cluster_ids, NR_CPUS);
struct sched_cluster *sched_cluster[NR_CPUS];
int num_clusters;

unsigned int max_power_cost = 1;

static struct sched_cluster init_cluster = {
	.list			=	LIST_HEAD_INIT(init_cluster.list),
	.id			=	0,
	.max_power_cost		=	1,
	.min_power_cost		=	1,
	.capacity		=	1024,
	.max_possible_capacity	=	1024,
	.efficiency		=	1,
	.load_scale_factor	=	1024,
	.cur_freq		=	1,
	.max_freq		=	1,
	.max_mitigated_freq	=	UINT_MAX,
	.min_freq		=	1,
	.max_possible_freq	=	1,
	.dstate			=	0,
	.dstate_wakeup_energy	=	0,
	.dstate_wakeup_latency	=	0,
	.exec_scale_factor	=	1024,
};

void update_all_clusters_stats(void)
{
	struct sched_cluster *cluster;
	u64 highest_mpc = 0, lowest_mpc = U64_MAX;

	pre_big_task_count_change(cpu_possible_mask);

	for_each_sched_cluster(cluster) {
		u64 mpc;

		cluster->capacity = compute_capacity(cluster);
		mpc = cluster->max_possible_capacity =
			compute_max_possible_capacity(cluster);
		cluster->load_scale_factor = compute_load_scale_factor(cluster);

		cluster->exec_scale_factor =
			DIV_ROUND_UP(cluster->efficiency * 1024,
				     max_possible_efficiency);

		if (mpc > highest_mpc)
			highest_mpc = mpc;

		if (mpc < lowest_mpc)
			lowest_mpc = mpc;
	}

	max_possible_capacity = highest_mpc;
	min_max_possible_capacity = lowest_mpc;

	__update_min_max_capacity();
	sched_update_freq_max_load(cpu_possible_mask);
	post_big_task_count_change(cpu_possible_mask);
}

static void assign_cluster_ids(struct list_head *head)
{
	struct sched_cluster *cluster;
	int pos = 0;

	list_for_each_entry(cluster, head, list) {
		cluster->id = pos;
		sched_cluster[pos++] = cluster;
	}
}

static void
move_list(struct list_head *dst, struct list_head *src, bool sync_rcu)
{
	struct list_head *first, *last;

	first = src->next;
	last = src->prev;

	if (sync_rcu) {
		INIT_LIST_HEAD_RCU(src);
		synchronize_rcu();
	}

	first->prev = dst;
	dst->prev = last;
	last->next = dst;

	/* Ensure list sanity before making the head visible to all CPUs. */
	smp_mb();
	dst->next = first;
}

static int
compare_clusters(void *priv, struct list_head *a, struct list_head *b)
{
	struct sched_cluster *cluster1, *cluster2;
	int ret;

	cluster1 = container_of(a, struct sched_cluster, list);
	cluster2 = container_of(b, struct sched_cluster, list);

	ret = cluster1->max_power_cost > cluster2->max_power_cost ||
		(cluster1->max_power_cost == cluster2->max_power_cost &&
		cluster1->max_possible_capacity <
				cluster2->max_possible_capacity);

	return ret;
}

static void sort_clusters(void)
{
	struct sched_cluster *cluster;
	struct list_head new_head;
	unsigned int tmp_max = 1;

	INIT_LIST_HEAD(&new_head);

	for_each_sched_cluster(cluster) {
		cluster->max_power_cost = power_cost(cluster_first_cpu(cluster),
							       max_task_load());
		cluster->min_power_cost = power_cost(cluster_first_cpu(cluster),
							       0);

		if (cluster->max_power_cost > tmp_max)
			tmp_max = cluster->max_power_cost;
	}
	max_power_cost = tmp_max;

	move_list(&new_head, &cluster_head, true);

	list_sort(NULL, &new_head, compare_clusters);
	assign_cluster_ids(&new_head);

	/*
	 * Ensure cluster ids are visible to all CPUs before making
	 * cluster_head visible.
	 */
	move_list(&cluster_head, &new_head, false);
}

static void
insert_cluster(struct sched_cluster *cluster, struct list_head *head)
{
	struct sched_cluster *tmp;
	struct list_head *iter = head;

	list_for_each_entry(tmp, head, list) {
		if (cluster->max_power_cost < tmp->max_power_cost)
			break;
		iter = &tmp->list;
	}

	list_add(&cluster->list, iter);
}

static struct sched_cluster *alloc_new_cluster(const struct cpumask *cpus)
{
	struct sched_cluster *cluster = NULL;

	cluster = kzalloc(sizeof(struct sched_cluster), GFP_ATOMIC);
	if (!cluster) {
		__WARN_printf("Cluster allocation failed. \
				Possible bad scheduling\n");
		return NULL;
	}

	INIT_LIST_HEAD(&cluster->list);
	cluster->max_power_cost		=	1;
	cluster->min_power_cost		=	1;
	cluster->capacity		=	1024;
	cluster->max_possible_capacity	=	1024;
	cluster->efficiency		=	1;
	cluster->load_scale_factor	=	1024;
	cluster->cur_freq		=	1;
	cluster->max_freq		=	1;
	cluster->max_mitigated_freq	=	UINT_MAX;
	cluster->min_freq		=	1;
	cluster->max_possible_freq	=	1;
	cluster->dstate			=	0;
	cluster->dstate_wakeup_energy	=	0;
	cluster->dstate_wakeup_latency	=	0;
	cluster->freq_init_done		=	false;

	cluster->cpus = *cpus;
	cluster->efficiency = arch_get_cpu_efficiency(cpumask_first(cpus));

	if (cluster->efficiency > max_possible_efficiency)
		max_possible_efficiency = cluster->efficiency;
	if (cluster->efficiency < min_possible_efficiency)
		min_possible_efficiency = cluster->efficiency;

	return cluster;
}

static void add_cluster(const struct cpumask *cpus, struct list_head *head)
{
	struct sched_cluster *cluster = alloc_new_cluster(cpus);
	int i;

	if (!cluster)
		return;

	for_each_cpu(i, cpus)
		cpu_rq(i)->cluster = cluster;

	insert_cluster(cluster, head);
	set_bit(num_clusters, all_cluster_ids);
	num_clusters++;
}

static void update_cluster_topology(void)
{
	struct cpumask cpus = *cpu_possible_mask;
	const struct cpumask *cluster_cpus;
	struct list_head new_head;
	int i;

	INIT_LIST_HEAD(&new_head);

	for_each_cpu(i, &cpus) {
		cluster_cpus = cpu_coregroup_mask(i);
		cpumask_or(&all_cluster_cpus, &all_cluster_cpus, cluster_cpus);
		cpumask_andnot(&cpus, &cpus, cluster_cpus);
		add_cluster(cluster_cpus, &new_head);
	}

	assign_cluster_ids(&new_head);

	/*
	 * Ensure cluster ids are visible to all CPUs before making
	 * cluster_head visible.
	 */
	move_list(&cluster_head, &new_head, false);
}

static void init_clusters(void)
{
	bitmap_clear(all_cluster_ids, 0, NR_CPUS);
	init_cluster.cpus = *cpu_possible_mask;
	INIT_LIST_HEAD(&cluster_head);
}

int register_cpu_cycle_counter_cb(struct cpu_cycle_counter_cb *cb)
{
	mutex_lock(&cluster_lock);
	if (!cb->get_cpu_cycle_counter) {
		mutex_unlock(&cluster_lock);
		return -EINVAL;
	}

	cpu_cycle_counter_cb = *cb;
	use_cycle_counter = true;
	mutex_unlock(&cluster_lock);

	return 0;
}

static int __init set_sched_enable_hmp(char *str)
{
	int enable_hmp = 0;

	get_option(&str, &enable_hmp);

	sched_enable_hmp = !!enable_hmp;

	return 0;
}

early_param("sched_enable_hmp", set_sched_enable_hmp);

static inline int got_boost_kick(void)
{
	int cpu = smp_processor_id();
	struct rq *rq = cpu_rq(cpu);

	return test_bit(BOOST_KICK, &rq->hmp_flags);
}

static inline void clear_boost_kick(int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	clear_bit(BOOST_KICK, &rq->hmp_flags);
}

void boost_kick(int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	if (!test_and_set_bit(BOOST_KICK, &rq->hmp_flags))
		smp_send_reschedule(cpu);
}

/* Clear any HMP scheduler related requests pending from or on cpu */
static inline void clear_hmp_request(int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	unsigned long flags;

	clear_boost_kick(cpu);
	clear_reserved(cpu);
	if (rq->push_task) {
		raw_spin_lock_irqsave(&rq->lock, flags);
		if (rq->push_task) {
			clear_reserved(rq->push_cpu);
			put_task_struct(rq->push_task);
			rq->push_task = NULL;
		}
		rq->active_balance = 0;
		raw_spin_unlock_irqrestore(&rq->lock, flags);
	}
}

int sched_set_static_cpu_pwr_cost(int cpu, unsigned int cost)
{
	struct rq *rq = cpu_rq(cpu);

	rq->static_cpu_pwr_cost = cost;
	return 0;
}

unsigned int sched_get_static_cpu_pwr_cost(int cpu)
{
	return cpu_rq(cpu)->static_cpu_pwr_cost;
}

int sched_set_static_cluster_pwr_cost(int cpu, unsigned int cost)
{
	struct sched_cluster *cluster = cpu_rq(cpu)->cluster;

	cluster->static_cluster_pwr_cost = cost;
	return 0;
}

unsigned int sched_get_static_cluster_pwr_cost(int cpu)
{
	return cpu_rq(cpu)->cluster->static_cluster_pwr_cost;
}

#else

static inline int got_boost_kick(void)
{
	return 0;
}

static inline void clear_boost_kick(int cpu) { }

static inline void clear_hmp_request(int cpu) { }

static inline void update_cluster_topology(void) {}

int register_cpu_cycle_counter_cb(struct cpu_cycle_counter_cb *cb)
{
	return 0;
}
#endif	/* CONFIG_SCHED_HMP */

#define SCHED_MIN_FREQ 1

#if defined(CONFIG_SCHED_HMP)

/*
 * sched_window_stats_policy and sched_ravg_hist_size have a 'sysctl' copy
 * associated with them. This is required for atomic update of those variables
 * when being modifed via sysctl interface.
 *
 * IMPORTANT: Initialize both copies to same value!!
 */

/*
 * Tasks that are runnable continuously for a period greather than
 * EARLY_DETECTION_DURATION can be flagged early as potential
 * high load tasks.
 */
#define EARLY_DETECTION_DURATION 9500000

static __read_mostly unsigned int sched_ravg_hist_size = 5;
__read_mostly unsigned int sysctl_sched_ravg_hist_size = 5;

static __read_mostly unsigned int sched_window_stats_policy =
	 WINDOW_STATS_MAX_RECENT_AVG;
__read_mostly unsigned int sysctl_sched_window_stats_policy =
	WINDOW_STATS_MAX_RECENT_AVG;

#define SCHED_ACCOUNT_WAIT_TIME 1

__read_mostly unsigned int sysctl_sched_cpu_high_irqload = (10 * NSEC_PER_MSEC);

unsigned int __read_mostly sysctl_sched_enable_colocation = 1;

/*
 * Enable colocation for all threads in a process. The children
 * inherits the group id from the parent.
 */
unsigned int __read_mostly sysctl_sched_enable_thread_grouping = 0;

#ifdef CONFIG_SCHED_FREQ_INPUT

__read_mostly unsigned int sysctl_sched_new_task_windows = 5;

#define SCHED_FREQ_ACCOUNT_WAIT_TIME 0

/*
 * For increase, send notification if
 *      freq_required - cur_freq > sysctl_sched_freq_inc_notify
 */
__read_mostly int sysctl_sched_freq_inc_notify = 10 * 1024 * 1024; /* + 10GHz */

/*
 * For decrease, send notification if
 *      cur_freq - freq_required > sysctl_sched_freq_dec_notify
 */
__read_mostly int sysctl_sched_freq_dec_notify = 10 * 1024 * 1024; /* - 10GHz */

static __read_mostly unsigned int sched_io_is_busy;

__read_mostly unsigned int sysctl_sched_pred_alert_freq = 10 * 1024 * 1024;

#endif	/* CONFIG_SCHED_FREQ_INPUT */

/* 1 -> use PELT based load stats, 0 -> use window-based load stats */
unsigned int __read_mostly sched_use_pelt;

/*
 * Maximum possible frequency across all cpus. Task demand and cpu
 * capacity (cpu_power) metrics are scaled in reference to it.
 */
unsigned int max_possible_freq = 1;

/*
 * Minimum possible max_freq across all cpus. This will be same as
 * max_possible_freq on homogeneous systems and could be different from
 * max_possible_freq on heterogenous systems. min_max_freq is used to derive
 * capacity (cpu_power) of cpus.
 */
unsigned int min_max_freq = 1;

unsigned int max_capacity = 1024; /* max(rq->capacity) */
unsigned int min_capacity = 1024; /* min(rq->capacity) */
unsigned int max_possible_capacity = 1024; /* max(rq->max_possible_capacity) */
unsigned int
min_max_possible_capacity = 1024; /* min(rq->max_possible_capacity) */

/* Window size (in ns) */
__read_mostly unsigned int sched_ravg_window = 10000000;

/* Min window size (in ns) = 10ms */
#define MIN_SCHED_RAVG_WINDOW 10000000

/* Max window size (in ns) = 1s */
#define MAX_SCHED_RAVG_WINDOW 1000000000

/* Temporarily disable window-stats activity on all cpus */
unsigned int __read_mostly sched_disable_window_stats;

/*
 * Major task runtime. If a task runs for more than sched_major_task_runtime
 * in a window, it's considered to be generating majority of workload
 * for this window. Prediction could be adjusted for such tasks.
 */
#ifdef CONFIG_SCHED_FREQ_INPUT
__read_mostly unsigned int sched_major_task_runtime = 10000000;

/*
 * Demand aggregation for frequency purpose:
 *
 * 'sched_freq_aggregate' controls aggregation of cpu demand of related threads
 * for frequency determination purpose. This aggregation is done per-cluster.
 *
 * CPU demand of tasks from various related groups is aggregated per-cluster and
 * added to the "max_busy_cpu" in that cluster, where max_busy_cpu is determined
 * by just rq->prev_runnable_sum.
 *
 * Some examples follow, which assume:
 *	Cluster0 = CPU0-3, Cluster1 = CPU4-7
 *	One related thread group A that has tasks A0, A1, A2
 *
 *	A->cpu_time[X].curr/prev_sum = counters in which cpu execution stats of
 *	tasks belonging to group A are accumulated when they run on cpu X.
 *
 *	CX->curr/prev_sum = counters in which cpu execution stats of all tasks
 *	not belonging to group A are accumulated when they run on cpu X
 *
 * Lets say the stats for window M was as below:
 *
 *	C0->prev_sum = 1ms, A->cpu_time[0].prev_sum = 5ms
 *		Task A0 ran 5ms on CPU0
 *		Task B0 ran 1ms on CPU0
 *
 *	C1->prev_sum = 5ms, A->cpu_time[1].prev_sum = 6ms
 *		Task A1 ran 4ms on CPU1
 *		Task A2 ran 2ms on CPU1
 *		Task B1 ran 5ms on CPU1
 *
 *	C2->prev_sum = 0ms, A->cpu_time[2].prev_sum = 0
 *		CPU2 idle
 *
 *	C3->prev_sum = 0ms, A->cpu_time[3].prev_sum = 0
 *		CPU3 idle
 *
 * In this case, CPU1 was most busy going by just its prev_sum counter. Demand
 * from all group A tasks are added to CPU1. IOW, at end of window M, cpu busy
 * time reported to governor will be:
 *
 *
 *	C0 busy time = 1ms
 *	C1 busy time = 5 + 5 + 6 = 16ms
 *
 */
static __read_mostly unsigned int sched_freq_aggregate;
__read_mostly unsigned int sysctl_sched_freq_aggregate;

#endif

static unsigned int sync_cpu;

static LIST_HEAD(related_thread_groups);
static DEFINE_RWLOCK(related_thread_group_lock);

#define for_each_related_thread_group(grp) \
	list_for_each_entry(grp, &related_thread_groups, list)

#define EXITING_TASK_MARKER	0xdeaddead

static inline int exiting_task(struct task_struct *p)
{
	return (p->ravg.sum_history[0] == EXITING_TASK_MARKER);
}

static int __init set_sched_ravg_window(char *str)
{
	get_option(&str, &sched_ravg_window);

	sched_use_pelt = (sched_ravg_window < MIN_SCHED_RAVG_WINDOW ||
				sched_ravg_window > MAX_SCHED_RAVG_WINDOW);

	return 0;
}

early_param("sched_ravg_window", set_sched_ravg_window);

static inline void
update_window_start(struct rq *rq, u64 wallclock)
{
	s64 delta;
	int nr_windows;

	delta = wallclock - rq->window_start;
	BUG_ON(delta < 0);
	if (delta < sched_ravg_window)
		return;

	nr_windows = div64_u64(delta, sched_ravg_window);
	rq->window_start += (u64)nr_windows * (u64)sched_ravg_window;
}

#define DIV64_U64_ROUNDUP(X, Y) div64_u64((X) + (Y - 1), Y)

static inline u64 scale_exec_time(u64 delta, struct rq *rq)
{
	u32 freq;

	freq = cpu_cycles_to_freq(rq->cc.cycles, rq->cc.time);
	delta = DIV64_U64_ROUNDUP(delta * freq, max_possible_freq);
	delta *= rq->cluster->exec_scale_factor;
	delta >>= 10;

	return delta;
}

static inline struct group_cpu_time *
_group_cpu_time(struct related_thread_group *grp, int cpu);

#ifdef CONFIG_SCHED_FREQ_INPUT

static inline int cpu_is_waiting_on_io(struct rq *rq)
{
	if (!sched_io_is_busy)
		return 0;

	return atomic_read(&rq->nr_iowait);
}

/* Does freq_required sufficiently exceed or fall behind cur_freq? */
static inline int
nearly_same_freq(unsigned int cur_freq, unsigned int freq_required)
{
	int delta = freq_required - cur_freq;

	if (freq_required > cur_freq)
		return delta < sysctl_sched_freq_inc_notify;

	delta = -delta;

	return delta < sysctl_sched_freq_dec_notify;
}

/* Convert busy time to frequency equivalent */
static inline unsigned int load_to_freq(struct rq *rq, u64 load)
{
	unsigned int freq;

	load = scale_load_to_cpu(load, cpu_of(rq));
	load *= 128;
	load = div64_u64(load, max_task_load());

	freq = load * cpu_max_possible_freq(cpu_of(rq));
	freq /= 128;

	return freq;
}

/*
 * Return load from all related group in given cpu.
 * Caller must ensure that related_thread_group_lock is held.
 */
static void _group_load_in_cpu(int cpu, u64 *grp_load, u64 *new_grp_load)
{
	struct related_thread_group *grp;

	for_each_related_thread_group(grp) {
		struct group_cpu_time *cpu_time;

		cpu_time = _group_cpu_time(grp, cpu);
		*grp_load += cpu_time->prev_runnable_sum;
		if (new_grp_load)
			*new_grp_load += cpu_time->nt_prev_runnable_sum;
	}
}

/*
 * Return load from all related groups in given frequency domain.
 * Caller must ensure that related_thread_group_lock is held.
 */
static void group_load_in_freq_domain(struct cpumask *cpus,
				u64 *grp_load, u64 *new_grp_load)
{
	struct related_thread_group *grp;
	int j;

	for_each_related_thread_group(grp) {
		for_each_cpu(j, cpus) {
			struct group_cpu_time *cpu_time;

			cpu_time = _group_cpu_time(grp, j);
			*grp_load += cpu_time->prev_runnable_sum;
			*new_grp_load += cpu_time->nt_prev_runnable_sum;
		}
	}
}

/*
 * Should scheduler alert governor for changing frequency?
 *
 * @check_pred - evaluate frequency based on the predictive demand
 * @check_groups - add load from all related groups on given cpu
 *
 * check_groups is set to 1 if a "related" task movement/wakeup is triggering
 * the notification check. To avoid "re-aggregation" of demand in such cases,
 * we check whether the migrated/woken tasks demand (along with demand from
 * existing tasks on the cpu) can be met on target cpu
 *
 */

static int send_notification(struct rq *rq, int check_pred, int check_groups)
{
	unsigned int cur_freq, freq_required;
	unsigned long flags;
	int rc = 0;
	u64 group_load = 0, new_load;

	if (!sched_enable_hmp)
		return 0;

	if (check_pred) {
		u64 prev = rq->old_busy_time;
		u64 predicted = rq->hmp_stats.pred_demands_sum;

		if (rq->cluster->cur_freq == cpu_max_freq(cpu_of(rq)))
			return 0;

		prev = max(prev, rq->old_estimated_time);
		if (prev > predicted)
			return 0;

		cur_freq = load_to_freq(rq, prev);
		freq_required = load_to_freq(rq, predicted);

		if (freq_required < cur_freq + sysctl_sched_pred_alert_freq)
			return 0;
	} else {
		read_lock(&related_thread_group_lock);
		/*
		 * Protect from concurrent update of rq->prev_runnable_sum and
		 * group cpu load
		 */
		raw_spin_lock_irqsave(&rq->lock, flags);
		if (check_groups)
			_group_load_in_cpu(cpu_of(rq), &group_load, NULL);

		new_load = rq->prev_runnable_sum + group_load;

		raw_spin_unlock_irqrestore(&rq->lock, flags);
		read_unlock(&related_thread_group_lock);

		cur_freq = load_to_freq(rq, rq->old_busy_time);
		freq_required = load_to_freq(rq, new_load);

		if (nearly_same_freq(cur_freq, freq_required))
			return 0;
	}

	raw_spin_lock_irqsave(&rq->lock, flags);
	if (!rq->notifier_sent) {
		rq->notifier_sent = 1;
		rc = 1;
		trace_sched_freq_alert(cpu_of(rq), check_pred, check_groups, rq,
				       new_load);
	}
	raw_spin_unlock_irqrestore(&rq->lock, flags);

	return rc;
}

/* Alert governor if there is a need to change frequency */
void check_for_freq_change(struct rq *rq, bool check_pred, bool check_groups)
{
	int cpu = cpu_of(rq);

	if (!send_notification(rq, check_pred, check_groups))
		return;

	atomic_notifier_call_chain(
		&load_alert_notifier_head, 0,
		(void *)(long)cpu);
}

static int account_busy_for_cpu_time(struct rq *rq, struct task_struct *p,
				     u64 irqtime, int event)
{
	if (is_idle_task(p)) {
		/* TASK_WAKE && TASK_MIGRATE is not possible on idle task! */
		if (event == PICK_NEXT_TASK)
			return 0;

		/* PUT_PREV_TASK, TASK_UPDATE && IRQ_UPDATE are left */
		return irqtime || cpu_is_waiting_on_io(rq);
	}

	if (event == TASK_WAKE)
		return 0;

	if (event == PUT_PREV_TASK || event == IRQ_UPDATE)
		return 1;

	/*
	 * TASK_UPDATE can be called on sleeping task, when its moved between
	 * related groups
	 */
	if (event == TASK_UPDATE) {
		if (rq->curr == p)
			return 1;

		return p->on_rq ? SCHED_FREQ_ACCOUNT_WAIT_TIME : 0;
	}

	/* TASK_MIGRATE, PICK_NEXT_TASK left */
	return SCHED_FREQ_ACCOUNT_WAIT_TIME;
}

static inline bool is_new_task(struct task_struct *p)
{
	return p->ravg.active_windows < sysctl_sched_new_task_windows;
}

#define INC_STEP 8
#define DEC_STEP 2
#define CONSISTENT_THRES 16
#define INC_STEP_BIG 16
/*
 * bucket_increase - update the count of all buckets
 *
 * @buckets: array of buckets tracking busy time of a task
 * @idx: the index of bucket to be incremented
 *
 * Each time a complete window finishes, count of bucket that runtime
 * falls in (@idx) is incremented. Counts of all other buckets are
 * decayed. The rate of increase and decay could be different based
 * on current count in the bucket.
 */
static inline void bucket_increase(u8 *buckets, int idx)
{
	int i, step;

	for (i = 0; i < NUM_BUSY_BUCKETS; i++) {
		if (idx != i) {
			if (buckets[i] > DEC_STEP)
				buckets[i] -= DEC_STEP;
			else
				buckets[i] = 0;
		} else {
			step = buckets[i] >= CONSISTENT_THRES ?
						INC_STEP_BIG : INC_STEP;
			if (buckets[i] > U8_MAX - step)
				buckets[i] = U8_MAX;
			else
				buckets[i] += step;
		}
	}
}

static inline int busy_to_bucket(u32 normalized_rt)
{
	int bidx;

	bidx = mult_frac(normalized_rt, NUM_BUSY_BUCKETS, max_task_load());
	bidx = min(bidx, NUM_BUSY_BUCKETS - 1);

	/*
	 * Combine lowest two buckets. The lowest frequency falls into
	 * 2nd bucket and thus keep predicting lowest bucket is not
	 * useful.
	 */
	if (!bidx)
		bidx++;

	return bidx;
}

static inline u64
scale_load_to_freq(u64 load, unsigned int src_freq, unsigned int dst_freq)
{
	return div64_u64(load * (u64)src_freq, (u64)dst_freq);
}

#define HEAVY_TASK_SKIP 2
#define HEAVY_TASK_SKIP_LIMIT 4
/*
 * get_pred_busy - calculate predicted demand for a task on runqueue
 *
 * @rq: runqueue of task p
 * @p: task whose prediction is being updated
 * @start: starting bucket. returned prediction should not be lower than
 *         this bucket.
 * @runtime: runtime of the task. returned prediction should not be lower
 *           than this runtime.
 * Note: @start can be derived from @runtime. It's passed in only to
 * avoid duplicated calculation in some cases.
 *
 * A new predicted busy time is returned for task @p based on @runtime
 * passed in. The function searches through buckets that represent busy
 * time equal to or bigger than @runtime and attempts to find the bucket to
 * to use for prediction. Once found, it searches through historical busy
 * time and returns the latest that falls into the bucket. If no such busy
 * time exists, it returns the medium of that bucket.
 */
static u32 get_pred_busy(struct rq *rq, struct task_struct *p,
				int start, u32 runtime)
{
	int i;
	u8 *buckets = p->ravg.busy_buckets;
	u32 *hist = p->ravg.sum_history;
	u32 dmin, dmax;
	u64 cur_freq_runtime = 0;
	int first = NUM_BUSY_BUCKETS, final, skip_to;
	u32 ret = runtime;

	/* skip prediction for new tasks due to lack of history */
	if (unlikely(is_new_task(p)))
		goto out;

	/* find minimal bucket index to pick */
	for (i = start; i < NUM_BUSY_BUCKETS; i++) {
		if (buckets[i]) {
			first = i;
			break;
		}
	}
	/* if no higher buckets are filled, predict runtime */
	if (first >= NUM_BUSY_BUCKETS)
		goto out;

	/* compute the bucket for prediction */
	final = first;
	if (first < HEAVY_TASK_SKIP_LIMIT) {
		/* compute runtime at current CPU frequency */
		cur_freq_runtime = mult_frac(runtime, max_possible_efficiency,
					     rq->cluster->efficiency);
		cur_freq_runtime = scale_load_to_freq(cur_freq_runtime,
				max_possible_freq, rq->cluster->cur_freq);
		/*
		 * if the task runs for majority of the window, try to
		 * pick higher buckets.
		 */
		if (cur_freq_runtime >= sched_major_task_runtime) {
			int next = NUM_BUSY_BUCKETS;
			/*
			 * if there is a higher bucket that's consistently
			 * hit, don't jump beyond that.
			 */
			for (i = start + 1; i <= HEAVY_TASK_SKIP_LIMIT &&
			     i < NUM_BUSY_BUCKETS; i++) {
				if (buckets[i] > CONSISTENT_THRES) {
					next = i;
					break;
				}
			}
			skip_to = min(next, start + HEAVY_TASK_SKIP);
			/* don't jump beyond HEAVY_TASK_SKIP_LIMIT */
			skip_to = min(HEAVY_TASK_SKIP_LIMIT, skip_to);
			/* don't go below first non-empty bucket, if any */
			final = max(first, skip_to);
		}
	}

	/* determine demand range for the predicted bucket */
	if (final < 2) {
		/* lowest two buckets are combined */
		dmin = 0;
		final = 1;
	} else {
		dmin = mult_frac(final, max_task_load(), NUM_BUSY_BUCKETS);
	}
	dmax = mult_frac(final + 1, max_task_load(), NUM_BUSY_BUCKETS);

	/*
	 * search through runtime history and return first runtime that falls
	 * into the range of predicted bucket.
	 */
	for (i = 0; i < sched_ravg_hist_size; i++) {
		if (hist[i] >= dmin && hist[i] < dmax) {
			ret = hist[i];
			break;
		}
	}
	/* no historical runtime within bucket found, use average of the bin */
	if (ret < dmin)
		ret = (dmin + dmax) / 2;
	/*
	 * when updating in middle of a window, runtime could be higher
	 * than all recorded history. Always predict at least runtime.
	 */
	ret = max(runtime, ret);
out:
	trace_sched_update_pred_demand(rq, p, runtime,
		mult_frac((unsigned int)cur_freq_runtime, 100,
			  sched_ravg_window), ret);
	return ret;
}

static inline u32 calc_pred_demand(struct rq *rq, struct task_struct *p)
{
	if (p->ravg.pred_demand >= p->ravg.curr_window)
		return p->ravg.pred_demand;

	return get_pred_busy(rq, p, busy_to_bucket(p->ravg.curr_window),
			     p->ravg.curr_window);
}

/*
 * predictive demand of a task is calculated at the window roll-over.
 * if the task current window busy time exceeds the predicted
 * demand, update it here to reflect the task needs.
 */
void update_task_pred_demand(struct rq *rq, struct task_struct *p, int event)
{
	u32 new, old;

	if (is_idle_task(p) || exiting_task(p))
		return;

	if (event != PUT_PREV_TASK && event != TASK_UPDATE &&
			(!SCHED_FREQ_ACCOUNT_WAIT_TIME ||
			 (event != TASK_MIGRATE &&
			 event != PICK_NEXT_TASK)))
		return;

	/*
	 * TASK_UPDATE can be called on sleeping task, when its moved between
	 * related groups
	 */
	if (event == TASK_UPDATE) {
		if (!p->on_rq && !SCHED_FREQ_ACCOUNT_WAIT_TIME)
			return;
	}

	new = calc_pred_demand(rq, p);
	old = p->ravg.pred_demand;

	if (old >= new)
		return;

	if (task_on_rq_queued(p) && (!task_has_dl_policy(p) ||
				!p->dl.dl_throttled))
		p->sched_class->fixup_hmp_sched_stats(rq, p,
				p->ravg.demand,
				new);

	p->ravg.pred_demand = new;
}

/*
 * Account cpu activity in its busy time counters (rq->curr/prev_runnable_sum)
 */
static void update_cpu_busy_time(struct task_struct *p, struct rq *rq,
				 int event, u64 wallclock, u64 irqtime)
{
	int new_window, full_window = 0;
	int p_is_curr_task = (p == rq->curr);
	u64 mark_start = p->ravg.mark_start;
	u64 window_start = rq->window_start;
	u32 window_size = sched_ravg_window;
	u64 delta;
	u64 *curr_runnable_sum = &rq->curr_runnable_sum;
	u64 *prev_runnable_sum = &rq->prev_runnable_sum;
	u64 *nt_curr_runnable_sum = &rq->nt_curr_runnable_sum;
	u64 *nt_prev_runnable_sum = &rq->nt_prev_runnable_sum;
	int flip_counters = 0;
	int prev_sum_reset = 0;
	bool new_task;
	struct related_thread_group *grp;

	new_window = mark_start < window_start;
	if (new_window) {
		full_window = (window_start - mark_start) >= window_size;
		if (p->ravg.active_windows < USHRT_MAX)
			p->ravg.active_windows++;
	}

	new_task = is_new_task(p);

	grp = p->grp;
	if (grp && sched_freq_aggregate) {
		/* cpu_time protected by rq_lock */
		struct group_cpu_time *cpu_time =
			_group_cpu_time(grp, cpu_of(rq));

		curr_runnable_sum = &cpu_time->curr_runnable_sum;
		prev_runnable_sum = &cpu_time->prev_runnable_sum;

		nt_curr_runnable_sum = &cpu_time->nt_curr_runnable_sum;
		nt_prev_runnable_sum = &cpu_time->nt_prev_runnable_sum;

		if (cpu_time->window_start != rq->window_start) {
			int nr_windows;

			delta = rq->window_start - cpu_time->window_start;
			nr_windows = div64_u64(delta, window_size);
			if (nr_windows > 1)
				prev_sum_reset = 1;

			cpu_time->window_start = rq->window_start;
			flip_counters = 1;
		}

		if (p_is_curr_task && new_window) {
			u64 curr_sum = rq->curr_runnable_sum;
			u64 nt_curr_sum = rq->nt_curr_runnable_sum;

			if (full_window)
				curr_sum = nt_curr_sum = 0;

			rq->prev_runnable_sum = curr_sum;
			rq->nt_prev_runnable_sum = nt_curr_sum;

			rq->curr_runnable_sum = 0;
			rq->nt_curr_runnable_sum = 0;
		}
	} else {
		if (p_is_curr_task && new_window) {
			flip_counters = 1;
			if (full_window)
				prev_sum_reset = 1;
		}
	}

	/* Handle per-task window rollover. We don't care about the idle
	 * task or exiting tasks. */
	if (new_window && !is_idle_task(p) && !exiting_task(p)) {
		u32 curr_window = 0;

		if (!full_window)
			curr_window = p->ravg.curr_window;

		p->ravg.prev_window = curr_window;
		p->ravg.curr_window = 0;
	}

	if (flip_counters) {
		u64 curr_sum = *curr_runnable_sum;
		u64 nt_curr_sum = *nt_curr_runnable_sum;

		if (prev_sum_reset)
			curr_sum = nt_curr_sum = 0;

		*prev_runnable_sum = curr_sum;
		*nt_prev_runnable_sum = nt_curr_sum;

		*curr_runnable_sum = 0;
		*nt_curr_runnable_sum = 0;
	}

	if (!account_busy_for_cpu_time(rq, p, irqtime, event)) {
		/* account_busy_for_cpu_time() = 0, so no update to the
		 * task's current window needs to be made. This could be
		 * for example
		 *
		 *   - a wakeup event on a task within the current
		 *     window (!new_window below, no action required),
		 *   - switching to a new task from idle (PICK_NEXT_TASK)
		 *     in a new window where irqtime is 0 and we aren't
		 *     waiting on IO */

		if (!new_window)
			return;

		/* A new window has started. The RQ demand must be rolled
		 * over if p is the current task. */
		if (p_is_curr_task) {
			/* p is idle task */
			BUG_ON(p != rq->idle);
		}

		return;
	}

	if (!new_window) {
		/* account_busy_for_cpu_time() = 1 so busy time needs
		 * to be accounted to the current window. No rollover
		 * since we didn't start a new window. An example of this is
		 * when a task starts execution and then sleeps within the
		 * same window. */

		if (!irqtime || !is_idle_task(p) || cpu_is_waiting_on_io(rq))
			delta = wallclock - mark_start;
		else
			delta = irqtime;
		delta = scale_exec_time(delta, rq);
		*curr_runnable_sum += delta;
		if (new_task)
			*nt_curr_runnable_sum += delta;

		if (!is_idle_task(p) && !exiting_task(p))
			p->ravg.curr_window += delta;

		return;
	}

	if (!p_is_curr_task) {
		/* account_busy_for_cpu_time() = 1 so busy time needs
		 * to be accounted to the current window. A new window
		 * has also started, but p is not the current task, so the
		 * window is not rolled over - just split up and account
		 * as necessary into curr and prev. The window is only
		 * rolled over when a new window is processed for the current
		 * task.
		 *
		 * Irqtime can't be accounted by a task that isn't the
		 * currently running task. */

		if (!full_window) {
			/* A full window hasn't elapsed, account partial
			 * contribution to previous completed window. */
			delta = scale_exec_time(window_start - mark_start, rq);
			if (!exiting_task(p))
				p->ravg.prev_window += delta;
		} else {
			/* Since at least one full window has elapsed,
			 * the contribution to the previous window is the
			 * full window (window_size). */
			delta = scale_exec_time(window_size, rq);
			if (!exiting_task(p))
				p->ravg.prev_window = delta;
		}

		*prev_runnable_sum += delta;
		if (new_task)
			*nt_prev_runnable_sum += delta;

		/* Account piece of busy time in the current window. */
		delta = scale_exec_time(wallclock - window_start, rq);
		*curr_runnable_sum += delta;
		if (new_task)
			*nt_curr_runnable_sum += delta;

		if (!exiting_task(p))
			p->ravg.curr_window = delta;

		return;
	}

	if (!irqtime || !is_idle_task(p) || cpu_is_waiting_on_io(rq)) {
		/* account_busy_for_cpu_time() = 1 so busy time needs
		 * to be accounted to the current window. A new window
		 * has started and p is the current task so rollover is
		 * needed. If any of these three above conditions are true
		 * then this busy time can't be accounted as irqtime.
		 *
		 * Busy time for the idle task or exiting tasks need not
		 * be accounted.
		 *
		 * An example of this would be a task that starts execution
		 * and then sleeps once a new window has begun. */

		if (!full_window) {
			/* A full window hasn't elapsed, account partial
			 * contribution to previous completed window. */
			delta = scale_exec_time(window_start - mark_start, rq);
			if (!is_idle_task(p) && !exiting_task(p))
				p->ravg.prev_window += delta;
		} else {
			/* Since at least one full window has elapsed,
			 * the contribution to the previous window is the
			 * full window (window_size). */
			delta = scale_exec_time(window_size, rq);
			if (!is_idle_task(p) && !exiting_task(p))
				p->ravg.prev_window = delta;
		}

		/* Rollover is done here by overwriting the values in
		 * prev_runnable_sum and curr_runnable_sum. */
		*prev_runnable_sum += delta;
		if (new_task)
			*nt_prev_runnable_sum += delta;

		/* Account piece of busy time in the current window. */
		delta = scale_exec_time(wallclock - window_start, rq);
		*curr_runnable_sum += delta;
		if (new_task)
			*nt_curr_runnable_sum += delta;

		if (!is_idle_task(p) && !exiting_task(p))
			p->ravg.curr_window = delta;

		return;
	}

	if (irqtime) {
		/* account_busy_for_cpu_time() = 1 so busy time needs
		 * to be accounted to the current window. A new window
		 * has started and p is the current task so rollover is
		 * needed. The current task must be the idle task because
		 * irqtime is not accounted for any other task.
		 *
		 * Irqtime will be accounted each time we process IRQ activity
		 * after a period of idleness, so we know the IRQ busy time
		 * started at wallclock - irqtime. */

		BUG_ON(!is_idle_task(p));
		mark_start = wallclock - irqtime;

		/* Roll window over. If IRQ busy time was just in the current
		 * window then that is all that need be accounted. */
		if (mark_start > window_start) {
			*curr_runnable_sum = scale_exec_time(irqtime, rq);
			return;
		}

		/* The IRQ busy time spanned multiple windows. Process the
		 * busy time preceding the current window start first. */
		delta = window_start - mark_start;
		if (delta > window_size)
			delta = window_size;
		delta = scale_exec_time(delta, rq);
		*prev_runnable_sum += delta;

		/* Process the remaining IRQ busy time in the current window. */
		delta = wallclock - window_start;
		rq->curr_runnable_sum = scale_exec_time(delta, rq);

		return;
	}

	BUG();
}

static inline u32 predict_and_update_buckets(struct rq *rq,
			struct task_struct *p, u32 runtime) {

	int bidx;
	u32 pred_demand;

	bidx = busy_to_bucket(runtime);
	pred_demand = get_pred_busy(rq, p, bidx, runtime);
	bucket_increase(p->ravg.busy_buckets, bidx);

	return pred_demand;
}
#define assign_ravg_pred_demand(x) (p->ravg.pred_demand = x)

#else	/* CONFIG_SCHED_FREQ_INPUT */

static inline void
update_task_pred_demand(struct rq *rq, struct task_struct *p, int event)
{
}

static inline void update_cpu_busy_time(struct task_struct *p, struct rq *rq,
	     int event, u64 wallclock, u64 irqtime)
{
}

static inline u32 predict_and_update_buckets(struct rq *rq,
			struct task_struct *p, u32 runtime)
{
	return 0;
}
#define assign_ravg_pred_demand(x)

#endif	/* CONFIG_SCHED_FREQ_INPUT */

u32 __weak get_freq_max_load(int cpu, u32 freq)
{
	/* 100% by default */
	return 100;
}

DEFINE_PER_CPU(struct freq_max_load *, freq_max_load);
static DEFINE_SPINLOCK(freq_max_load_lock);

int sched_update_freq_max_load(const cpumask_t *cpumask)
{
	int i, cpu, ret;
	unsigned int freq;
	struct cpu_pstate_pwr *costs;
	struct cpu_pwr_stats *per_cpu_info = get_cpu_pwr_stats();
	struct freq_max_load *max_load, *old_max_load;
	struct freq_max_load_entry *entry;
	u64 max_demand_capacity, max_demand;
	unsigned long flags;
	u32 hfreq;
	int hpct;

	if (!per_cpu_info)
		return 0;

	spin_lock_irqsave(&freq_max_load_lock, flags);
	max_demand_capacity = div64_u64(max_task_load(), max_possible_capacity);
	for_each_cpu(cpu, cpumask) {
		if (!per_cpu_info[cpu].ptable) {
			ret = -EINVAL;
			goto fail;
		}

		old_max_load = rcu_dereference(per_cpu(freq_max_load, cpu));

		/*
		 * allocate len + 1 and leave the last power cost as 0 for
		 * power_cost() can stop iterating index when
		 * per_cpu_info[cpu].len > len of max_load due to race between
		 * cpu power stats update and get_cpu_pwr_stats().
		 */
		max_load = kzalloc(sizeof(struct freq_max_load) +
				   sizeof(struct freq_max_load_entry) *
				   (per_cpu_info[cpu].len + 1), GFP_ATOMIC);
		if (unlikely(!max_load)) {
			ret = -ENOMEM;
			goto fail;
		}

		max_load->length = per_cpu_info[cpu].len;

		max_demand = max_demand_capacity *
			     cpu_max_possible_capacity(cpu);

		i = 0;
		costs = per_cpu_info[cpu].ptable;
		while (costs[i].freq) {
			entry = &max_load->freqs[i];
			freq = costs[i].freq;
			hpct = get_freq_max_load(cpu, freq);
			if (hpct <= 0 && hpct > 100)
				hpct = 100;
			hfreq = div64_u64((u64)freq * hpct , 100);
			entry->hdemand =
			    div64_u64(max_demand * hfreq,
				      cpu_max_possible_freq(cpu));
			i++;
		}

		rcu_assign_pointer(per_cpu(freq_max_load, cpu), max_load);
		if (old_max_load)
			kfree_rcu(old_max_load, rcu);
	}

	spin_unlock_irqrestore(&freq_max_load_lock, flags);
	return 0;

fail:
	for_each_cpu(cpu, cpumask) {
		max_load = rcu_dereference(per_cpu(freq_max_load, cpu));
		if (max_load) {
			rcu_assign_pointer(per_cpu(freq_max_load, cpu), NULL);
			kfree_rcu(max_load, rcu);
		}
	}

	spin_unlock_irqrestore(&freq_max_load_lock, flags);
	return ret;
}

static void update_task_cpu_cycles(struct task_struct *p, int cpu)
{
	if (use_cycle_counter)
		p->cpu_cycles = cpu_cycle_counter_cb.get_cpu_cycle_counter(cpu);
}

static void
update_task_rq_cpu_cycles(struct task_struct *p, struct rq *rq, int event,
			  u64 wallclock, u64 irqtime)
{
	u64 cur_cycles;
	int cpu = cpu_of(rq);

	lockdep_assert_held(&rq->lock);

	if (!use_cycle_counter) {
		rq->cc.cycles = cpu_cur_freq(cpu);
		rq->cc.time = 1;
		return;
	}

	cur_cycles = cpu_cycle_counter_cb.get_cpu_cycle_counter(cpu);

	/*
	 * If current task is idle task and irqtime == 0 CPU was
	 * indeed idle and probably its cycle counter was not
	 * increasing.  We still need estimatied CPU frequency
	 * for IO wait time accounting.  Use the previously
	 * calculated frequency in such a case.
	 */
	if (!is_idle_task(rq->curr) || irqtime) {
		if (unlikely(cur_cycles < p->cpu_cycles))
			rq->cc.cycles = cur_cycles + (U64_MAX - p->cpu_cycles);
		else
			rq->cc.cycles = cur_cycles - p->cpu_cycles;
		rq->cc.cycles = rq->cc.cycles * NSEC_PER_MSEC;

		if (event == IRQ_UPDATE && is_idle_task(p))
			/*
			 * Time between mark_start of idle task and IRQ handler
			 * entry time is CPU cycle counter stall period.
			 * Upon IRQ handler entry sched_account_irqstart()
			 * replenishes idle task's cpu cycle counter so
			 * rq->cc.cycles now represents increased cycles during
			 * IRQ handler rather than time between idle entry and
			 * IRQ exit.  Thus use irqtime as time delta.
			 */
			rq->cc.time = irqtime;
		else
			rq->cc.time = wallclock - p->ravg.mark_start;
		BUG_ON((s64)rq->cc.time < 0);
	}

	p->cpu_cycles = cur_cycles;

	trace_sched_get_task_cpu_cycles(cpu, event, rq->cc.cycles, rq->cc.time);
}

static int account_busy_for_task_demand(struct task_struct *p, int event)
{
	/* No need to bother updating task demand for exiting tasks
	 * or the idle task. */
	if (exiting_task(p) || is_idle_task(p))
		return 0;

	/* When a task is waking up it is completing a segment of non-busy
	 * time. Likewise, if wait time is not treated as busy time, then
	 * when a task begins to run or is migrated, it is not running and
	 * is completing a segment of non-busy time. */
	if (event == TASK_WAKE || (!SCHED_ACCOUNT_WAIT_TIME &&
			 (event == PICK_NEXT_TASK || event == TASK_MIGRATE)))
		return 0;

	return 1;
}

/*
 * Called when new window is starting for a task, to record cpu usage over
 * recently concluded window(s). Normally 'samples' should be 1. It can be > 1
 * when, say, a real-time task runs without preemption for several windows at a
 * stretch.
 */
static void update_history(struct rq *rq, struct task_struct *p,
			 u32 runtime, int samples, int event)
{
	u32 *hist = &p->ravg.sum_history[0];
	int ridx, widx;
	u32 max = 0, avg, demand, pred_demand;
	u64 sum = 0;

	/* Ignore windows where task had no activity */
	if (!runtime || is_idle_task(p) || exiting_task(p) || !samples)
			goto done;

	/* Push new 'runtime' value onto stack */
	widx = sched_ravg_hist_size - 1;
	ridx = widx - samples;
	for (; ridx >= 0; --widx, --ridx) {
		hist[widx] = hist[ridx];
		sum += hist[widx];
		if (hist[widx] > max)
			max = hist[widx];
	}

	for (widx = 0; widx < samples && widx < sched_ravg_hist_size; widx++) {
		hist[widx] = runtime;
		sum += hist[widx];
		if (hist[widx] > max)
			max = hist[widx];
	}

	p->ravg.sum = 0;

	if (sched_window_stats_policy == WINDOW_STATS_RECENT) {
		demand = runtime;
	} else if (sched_window_stats_policy == WINDOW_STATS_MAX) {
		demand = max;
	} else {
		avg = div64_u64(sum, sched_ravg_hist_size);
		if (sched_window_stats_policy == WINDOW_STATS_AVG)
			demand = avg;
		else
			demand = max(avg, runtime);
	}
	pred_demand = predict_and_update_buckets(rq, p, runtime);

	/*
	 * A throttled deadline sched class task gets dequeued without
	 * changing p->on_rq. Since the dequeue decrements hmp stats
	 * avoid decrementing it here again.
	 */
	if (task_on_rq_queued(p) && (!task_has_dl_policy(p) ||
						!p->dl.dl_throttled))
		p->sched_class->fixup_hmp_sched_stats(rq, p, demand,
						      pred_demand);

	p->ravg.demand = demand;
	assign_ravg_pred_demand(pred_demand);

done:
	trace_sched_update_history(rq, p, runtime, samples, event);
}

static void add_to_task_demand(struct rq *rq, struct task_struct *p, u64 delta)
{
	delta = scale_exec_time(delta, rq);
	p->ravg.sum += delta;
	if (unlikely(p->ravg.sum > sched_ravg_window))
		p->ravg.sum = sched_ravg_window;
}

/*
 * Account cpu demand of task and/or update task's cpu demand history
 *
 * ms = p->ravg.mark_start;
 * wc = wallclock
 * ws = rq->window_start
 *
 * Three possibilities:
 *
 *	a) Task event is contained within one window.
 *		window_start < mark_start < wallclock
 *
 *		ws   ms  wc
 *		|    |   |
 *		V    V   V
 *		|---------------|
 *
 *	In this case, p->ravg.sum is updated *iff* event is appropriate
 *	(ex: event == PUT_PREV_TASK)
 *
 *	b) Task event spans two windows.
 *		mark_start < window_start < wallclock
 *
 *		ms   ws   wc
 *		|    |    |
 *		V    V    V
 *		-----|-------------------
 *
 *	In this case, p->ravg.sum is updated with (ws - ms) *iff* event
 *	is appropriate, then a new window sample is recorded followed
 *	by p->ravg.sum being set to (wc - ws) *iff* event is appropriate.
 *
 *	c) Task event spans more than two windows.
 *
 *		ms ws_tmp			   ws  wc
 *		|  |				   |   |
 *		V  V				   V   V
 *		---|-------|-------|-------|-------|------
 *		   |				   |
 *		   |<------ nr_full_windows ------>|
 *
 *	In this case, p->ravg.sum is updated with (ws_tmp - ms) first *iff*
 *	event is appropriate, window sample of p->ravg.sum is recorded,
 *	'nr_full_window' samples of window_size is also recorded *iff*
 *	event is appropriate and finally p->ravg.sum is set to (wc - ws)
 *	*iff* event is appropriate.
 *
 * IMPORTANT : Leave p->ravg.mark_start unchanged, as update_cpu_busy_time()
 * depends on it!
 */
static void update_task_demand(struct task_struct *p, struct rq *rq,
			       int event, u64 wallclock)
{
	u64 mark_start = p->ravg.mark_start;
	u64 delta, window_start = rq->window_start;
	int new_window, nr_full_windows;
	u32 window_size = sched_ravg_window;

	new_window = mark_start < window_start;
	if (!account_busy_for_task_demand(p, event)) {
		if (new_window)
			/* If the time accounted isn't being accounted as
			 * busy time, and a new window started, only the
			 * previous window need be closed out with the
			 * pre-existing demand. Multiple windows may have
			 * elapsed, but since empty windows are dropped,
			 * it is not necessary to account those. */
			update_history(rq, p, p->ravg.sum, 1, event);
		return;
	}

	if (!new_window) {
		/* The simple case - busy time contained within the existing
		 * window. */
		add_to_task_demand(rq, p, wallclock - mark_start);
		return;
	}

	/* Busy time spans at least two windows. Temporarily rewind
	 * window_start to first window boundary after mark_start. */
	delta = window_start - mark_start;
	nr_full_windows = div64_u64(delta, window_size);
	window_start -= (u64)nr_full_windows * (u64)window_size;

	/* Process (window_start - mark_start) first */
	add_to_task_demand(rq, p, window_start - mark_start);

	/* Push new sample(s) into task's demand history */
	update_history(rq, p, p->ravg.sum, 1, event);
	if (nr_full_windows)
		update_history(rq, p, scale_exec_time(window_size, rq),
			       nr_full_windows, event);

	/* Roll window_start back to current to process any remainder
	 * in current window. */
	window_start += (u64)nr_full_windows * (u64)window_size;

	/* Process (wallclock - window_start) next */
	mark_start = window_start;
	add_to_task_demand(rq, p, wallclock - mark_start);
}

/* Reflect task activity on its demand and cpu's busy time statistics */
static void
update_task_ravg(struct task_struct *p, struct rq *rq, int event,
		 u64 wallclock, u64 irqtime)
{
	if (sched_use_pelt || !rq->window_start || sched_disable_window_stats)
		return;

	lockdep_assert_held(&rq->lock);

	update_window_start(rq, wallclock);

	if (!p->ravg.mark_start) {
		update_task_cpu_cycles(p, cpu_of(rq));
		goto done;
	}

	update_task_rq_cpu_cycles(p, rq, event, wallclock, irqtime);
	update_task_demand(p, rq, event, wallclock);
	update_cpu_busy_time(p, rq, event, wallclock, irqtime);
	update_task_pred_demand(rq, p, event);
done:
	trace_sched_update_task_ravg(p, rq, event, wallclock, irqtime,
				     rq->cc.cycles, rq->cc.time,
				     _group_cpu_time(p->grp, cpu_of(rq)));

	p->ravg.mark_start = wallclock;
}

void sched_account_irqtime(int cpu, struct task_struct *curr,
				 u64 delta, u64 wallclock)
{
	struct rq *rq = cpu_rq(cpu);
	unsigned long flags, nr_windows;
	u64 cur_jiffies_ts;

	raw_spin_lock_irqsave(&rq->lock, flags);

	/*
	 * cputime (wallclock) uses sched_clock so use the same here for
	 * consistency.
	 */
	delta += sched_clock() - wallclock;
	cur_jiffies_ts = get_jiffies_64();

	if (is_idle_task(curr))
		update_task_ravg(curr, rq, IRQ_UPDATE, sched_ktime_clock(),
				 delta);

	nr_windows = cur_jiffies_ts - rq->irqload_ts;

	if (nr_windows) {
		if (nr_windows < 10) {
			/* Decay CPU's irqload by 3/4 for each window. */
			rq->avg_irqload *= (3 * nr_windows);
			rq->avg_irqload = div64_u64(rq->avg_irqload,
						    4 * nr_windows);
		} else {
			rq->avg_irqload = 0;
		}
		rq->avg_irqload += rq->cur_irqload;
		rq->cur_irqload = 0;
	}

	rq->cur_irqload += delta;
	rq->irqload_ts = cur_jiffies_ts;
	raw_spin_unlock_irqrestore(&rq->lock, flags);
}

void sched_account_irqstart(int cpu, struct task_struct *curr, u64 wallclock)
{
	struct rq *rq = cpu_rq(cpu);

	if (!rq->window_start || sched_disable_window_stats)
		return;

	if (is_idle_task(curr)) {
		/* We're here without rq->lock held, IRQ disabled */
		raw_spin_lock(&rq->lock);
		update_task_cpu_cycles(curr, cpu);
		raw_spin_unlock(&rq->lock);
	}
}

static void reset_task_stats(struct task_struct *p)
{
	u32 sum = 0;

	if (exiting_task(p))
		sum = EXITING_TASK_MARKER;

	memset(&p->ravg, 0, sizeof(struct ravg));
	/* Retain EXITING_TASK marker */
	p->ravg.sum_history[0] = sum;
}

static inline void mark_task_starting(struct task_struct *p)
{
	u64 wallclock;
	struct rq *rq = task_rq(p);

	if (!rq->window_start || sched_disable_window_stats) {
		reset_task_stats(p);
		return;
	}

	wallclock = sched_ktime_clock();
	p->ravg.mark_start = p->last_wake_ts = wallclock;
	p->last_cpu_selected_ts = wallclock;
	p->last_switch_out_ts = 0;
	update_task_cpu_cycles(p, cpu_of(rq));
}

static inline void set_window_start(struct rq *rq)
{
	int cpu = cpu_of(rq);
	struct rq *sync_rq = cpu_rq(sync_cpu);

	if (rq->window_start || !sched_enable_hmp)
		return;

	if (cpu == sync_cpu) {
		rq->window_start = sched_ktime_clock();
	} else {
		raw_spin_unlock(&rq->lock);
		double_rq_lock(rq, sync_rq);
		rq->window_start = cpu_rq(sync_cpu)->window_start;
#ifdef CONFIG_SCHED_FREQ_INPUT
		rq->curr_runnable_sum = rq->prev_runnable_sum = 0;
		rq->nt_curr_runnable_sum = rq->nt_prev_runnable_sum = 0;
#endif
		raw_spin_unlock(&sync_rq->lock);
	}

	rq->curr->ravg.mark_start = rq->window_start;
}

static inline void migrate_sync_cpu(int cpu)
{
	if (cpu == sync_cpu)
		sync_cpu = smp_processor_id();
}

static void reset_all_task_stats(void)
{
	struct task_struct *g, *p;

	read_lock(&tasklist_lock);
	do_each_thread(g, p) {
		reset_task_stats(p);
	}  while_each_thread(g, p);
	read_unlock(&tasklist_lock);
}

/*
 * sched_exit() - Set EXITING_TASK_MARKER in task's ravg.demand field
 *
 * Stop accounting (exiting) task's future cpu usage
 *
 * We need this so that reset_all_windows_stats() can function correctly.
 * reset_all_window_stats() depends on do_each_thread/for_each_thread task
 * iterators to reset *all* task's statistics. Exiting tasks however become
 * invisible to those iterators. sched_exit() is called on a exiting task prior
 * to being removed from task_list, which will let reset_all_window_stats()
 * function correctly.
 */
void sched_exit(struct task_struct *p)
{
	unsigned long flags;
	int cpu = get_cpu();
	struct rq *rq = cpu_rq(cpu);
	u64 wallclock;

	sched_set_group_id(p, 0);

	raw_spin_lock_irqsave(&rq->lock, flags);
	/* rq->curr == p */
	wallclock = sched_ktime_clock();
	update_task_ravg(rq->curr, rq, TASK_UPDATE, wallclock, 0);
	dequeue_task(rq, p, 0);
	reset_task_stats(p);
	p->ravg.mark_start = wallclock;
	p->ravg.sum_history[0] = EXITING_TASK_MARKER;
	enqueue_task(rq, p, 0);
	clear_ed_task(p, rq);
	raw_spin_unlock_irqrestore(&rq->lock, flags);

	put_cpu();
}

static void disable_window_stats(void)
{
	unsigned long flags;
	int i;

	local_irq_save(flags);
	for_each_possible_cpu(i)
		raw_spin_lock(&cpu_rq(i)->lock);

	sched_disable_window_stats = 1;

	for_each_possible_cpu(i)
		raw_spin_unlock(&cpu_rq(i)->lock);

	local_irq_restore(flags);
}

/* Called with all cpu's rq->lock held */
static void enable_window_stats(void)
{
	sched_disable_window_stats = 0;

}

enum reset_reason_code {
	WINDOW_CHANGE,
	POLICY_CHANGE,
	HIST_SIZE_CHANGE,
	FREQ_AGGREGATE_CHANGE,
};

const char *sched_window_reset_reasons[] = {
	"WINDOW_CHANGE",
	"POLICY_CHANGE",
	"HIST_SIZE_CHANGE",
};

static inline void _reset_all_group_time(u64 window_start);
/* Called with IRQs enabled */
void reset_all_window_stats(u64 window_start, unsigned int window_size)
{
	int cpu;
	unsigned long flags;
	u64 start_ts = sched_ktime_clock();
	int reason = WINDOW_CHANGE;
	unsigned int old = 0, new = 0;

	disable_window_stats();

	reset_all_task_stats();

	local_irq_save(flags);

	read_lock(&related_thread_group_lock);

	for_each_possible_cpu(cpu) {
		struct rq *rq = cpu_rq(cpu);
		raw_spin_lock(&rq->lock);
	}

	_reset_all_group_time(window_start);

	if (window_size) {
		sched_ravg_window = window_size * TICK_NSEC;
		set_hmp_defaults();
	}

	enable_window_stats();

	for_each_possible_cpu(cpu) {
		struct rq *rq = cpu_rq(cpu);

		if (window_start)
			rq->window_start = window_start;
#ifdef CONFIG_SCHED_FREQ_INPUT
		rq->curr_runnable_sum = rq->prev_runnable_sum = 0;
		rq->nt_curr_runnable_sum = rq->nt_prev_runnable_sum = 0;
#endif
		reset_cpu_hmp_stats(cpu, 1);
	}

	if (sched_window_stats_policy != sysctl_sched_window_stats_policy) {
		reason = POLICY_CHANGE;
		old = sched_window_stats_policy;
		new = sysctl_sched_window_stats_policy;
		sched_window_stats_policy = sysctl_sched_window_stats_policy;
	} else if (sched_ravg_hist_size != sysctl_sched_ravg_hist_size) {
		reason = HIST_SIZE_CHANGE;
		old = sched_ravg_hist_size;
		new = sysctl_sched_ravg_hist_size;
		sched_ravg_hist_size = sysctl_sched_ravg_hist_size;
	}
#ifdef CONFIG_SCHED_FREQ_INPUT
	else if (sched_freq_aggregate !=
					sysctl_sched_freq_aggregate) {
		reason = FREQ_AGGREGATE_CHANGE;
		old = sched_freq_aggregate;
		new = sysctl_sched_freq_aggregate;
		sched_freq_aggregate = sysctl_sched_freq_aggregate;
	}
#endif

	for_each_possible_cpu(cpu) {
		struct rq *rq = cpu_rq(cpu);
		raw_spin_unlock(&rq->lock);
	}

	read_unlock(&related_thread_group_lock);

	local_irq_restore(flags);

	trace_sched_reset_all_window_stats(window_start, window_size,
		sched_ktime_clock() - start_ts, reason, old, new);
}

#ifdef CONFIG_SCHED_FREQ_INPUT

static inline void
sync_window_start(struct rq *rq, struct group_cpu_time *cpu_time);

void sched_get_cpus_busy(struct sched_load *busy,
			 const struct cpumask *query_cpus)
{
	unsigned long flags;
	struct rq *rq;
	const int cpus = cpumask_weight(query_cpus);
	u64 load[cpus], group_load[cpus];
	u64 nload[cpus], ngload[cpus];
	u64 pload[cpus];
	unsigned int cur_freq[cpus], max_freq[cpus];
	int notifier_sent[cpus];
	int early_detection[cpus];
	int cpu, i = 0;
	unsigned int window_size;
	u64 max_prev_sum = 0;
	int max_busy_cpu = cpumask_first(query_cpus);
	struct related_thread_group *grp;
	u64 total_group_load = 0, total_ngload = 0;
	bool aggregate_load = false;

	if (unlikely(cpus == 0))
		return;

	/*
	 * This function could be called in timer context, and the
	 * current task may have been executing for a long time. Ensure
	 * that the window stats are current by doing an update.
	 */
	read_lock(&related_thread_group_lock);

	local_irq_save(flags);
	for_each_cpu(cpu, query_cpus)
		raw_spin_lock(&cpu_rq(cpu)->lock);

	window_size = sched_ravg_window;

	for_each_cpu(cpu, query_cpus) {
		rq = cpu_rq(cpu);

		update_task_ravg(rq->curr, rq, TASK_UPDATE, sched_ktime_clock(),
				 0);
		cur_freq[i] = cpu_cycles_to_freq(rq->cc.cycles, rq->cc.time);

		load[i] = rq->old_busy_time = rq->prev_runnable_sum;
		nload[i] = rq->nt_prev_runnable_sum;
		pload[i] = rq->hmp_stats.pred_demands_sum;
		rq->old_estimated_time = pload[i];

		if (load[i] > max_prev_sum) {
			max_prev_sum = load[i];
			max_busy_cpu = cpu;
		}

		notifier_sent[i] = rq->notifier_sent;
		early_detection[i] = (rq->ed_task != NULL);
		rq->notifier_sent = 0;
		cur_freq[i] = cpu_cur_freq(cpu);
		max_freq[i] = cpu_max_freq(cpu);
		i++;
	}

	for_each_related_thread_group(grp) {
		for_each_cpu(cpu, query_cpus) {
			/* Protected by rq_lock */
			struct group_cpu_time *cpu_time =
						_group_cpu_time(grp, cpu);
			sync_window_start(cpu_rq(cpu), cpu_time);
		}
	}

	if (!notifier_sent[max_busy_cpu]) {
		group_load_in_freq_domain(
				&cpu_rq(max_busy_cpu)->freq_domain_cpumask,
				&total_group_load, &total_ngload);
		if (total_group_load > sched_freq_aggregate_threshold)
			aggregate_load = true;
	}

	i = 0;
	for_each_cpu(cpu, query_cpus) {
		group_load[i] = 0;
		ngload[i] = 0;

		if (early_detection[i])
			goto skip_early;

		rq = cpu_rq(cpu);
		if (!notifier_sent[i] && aggregate_load) {
			if (cpu == max_busy_cpu) {
				group_load[i] = total_group_load;
				ngload[i] = total_ngload;
			}
		} else {
			_group_load_in_cpu(cpu, &group_load[i], &ngload[i]);
		}

		load[i] += group_load[i];
		nload[i] += ngload[i];
		/*
		 * Scale load in reference to cluster max_possible_freq.
		 *
		 * Note that scale_load_to_cpu() scales load in reference to
		 * the cluster max_freq.
		 */
		load[i] = scale_load_to_cpu(load[i], cpu);
		nload[i] = scale_load_to_cpu(nload[i], cpu);
		pload[i] = scale_load_to_cpu(pload[i], cpu);
skip_early:
		i++;
	}

	for_each_cpu(cpu, query_cpus)
		raw_spin_unlock(&(cpu_rq(cpu))->lock);
	local_irq_restore(flags);

	read_unlock(&related_thread_group_lock);

	i = 0;
	for_each_cpu(cpu, query_cpus) {
		rq = cpu_rq(cpu);

		if (early_detection[i]) {
			busy[i].prev_load = div64_u64(sched_ravg_window,
							NSEC_PER_USEC);
			busy[i].new_task_load = 0;
			goto exit_early;
		}

		/*
		 * When the load aggregation is controlled by
		 * sched_freq_aggregate_threshold, allow reporting loads
		 * greater than 100 @ Fcur to ramp up the frequency
		 * faster.
		 */
		if (notifier_sent[i] || (aggregate_load &&
					sched_freq_aggregate_threshold)) {
			load[i] = scale_load_to_freq(load[i], max_freq[i],
						    cpu_max_possible_freq(cpu));
			nload[i] = scale_load_to_freq(nload[i], max_freq[i],
						    cpu_max_possible_freq(cpu));
		} else {
			load[i] = scale_load_to_freq(load[i], max_freq[i],
						     cur_freq[i]);
			nload[i] = scale_load_to_freq(nload[i], max_freq[i],
						      cur_freq[i]);
			if (load[i] > window_size)
				load[i] = window_size;
			if (nload[i] > window_size)
				nload[i] = window_size;

			load[i] = scale_load_to_freq(load[i], cur_freq[i],
						    cpu_max_possible_freq(cpu));
			nload[i] = scale_load_to_freq(nload[i], cur_freq[i],
						    cpu_max_possible_freq(cpu));
		}
		pload[i] = scale_load_to_freq(pload[i], max_freq[i],
					     rq->cluster->max_possible_freq);

		busy[i].prev_load = div64_u64(load[i], NSEC_PER_USEC);
		busy[i].new_task_load = div64_u64(nload[i], NSEC_PER_USEC);
		busy[i].predicted_load = div64_u64(pload[i], NSEC_PER_USEC);

exit_early:
		trace_sched_get_busy(cpu, busy[i].prev_load,
				     busy[i].new_task_load,
				     busy[i].predicted_load,
				     early_detection[i]);
		i++;
	}
}

void sched_set_io_is_busy(int val)
{
	sched_io_is_busy = val;
}

int sched_set_window(u64 window_start, unsigned int window_size)
{
	u64 now, cur_jiffies, jiffy_ktime_ns;
	s64 ws;
	unsigned long flags;

	if (sched_use_pelt ||
		 (window_size * TICK_NSEC <  MIN_SCHED_RAVG_WINDOW))
			return -EINVAL;

	mutex_lock(&policy_mutex);

	/*
	 * Get a consistent view of ktime, jiffies, and the time
	 * since the last jiffy (based on last_jiffies_update).
	 */
	local_irq_save(flags);
	cur_jiffies = jiffy_to_ktime_ns(&now, &jiffy_ktime_ns);
	local_irq_restore(flags);

	/* translate window_start from jiffies to nanoseconds */
	ws = (window_start - cur_jiffies); /* jiffy difference */
	ws *= TICK_NSEC;
	ws += jiffy_ktime_ns;

	/* roll back calculated window start so that it is in
	 * the past (window stats must have a current window) */
	while (ws > now)
		ws -= (window_size * TICK_NSEC);

	BUG_ON(sched_ktime_clock() < ws);

	reset_all_window_stats(ws, window_size);

	sched_update_freq_max_load(cpu_possible_mask);

	mutex_unlock(&policy_mutex);

	return 0;
}

static void fixup_busy_time(struct task_struct *p, int new_cpu)
{
	struct rq *src_rq = task_rq(p);
	struct rq *dest_rq = cpu_rq(new_cpu);
	u64 wallclock;
	u64 *src_curr_runnable_sum, *dst_curr_runnable_sum;
	u64 *src_prev_runnable_sum, *dst_prev_runnable_sum;
	u64 *src_nt_curr_runnable_sum, *dst_nt_curr_runnable_sum;
	u64 *src_nt_prev_runnable_sum, *dst_nt_prev_runnable_sum;
	int migrate_type;
	struct migration_sum_data d;
	bool new_task;
	struct related_thread_group *grp;

	if (!sched_enable_hmp || (!p->on_rq && p->state != TASK_WAKING))
		return;

	if (exiting_task(p)) {
		clear_ed_task(p, src_rq);
		return;
	}

	if (p->state == TASK_WAKING)
		double_rq_lock(src_rq, dest_rq);

	if (sched_disable_window_stats)
		goto done;

	wallclock = sched_ktime_clock();

	update_task_ravg(task_rq(p)->curr, task_rq(p),
			 TASK_UPDATE,
			 wallclock, 0);
	update_task_ravg(dest_rq->curr, dest_rq,
			 TASK_UPDATE, wallclock, 0);

	update_task_ravg(p, task_rq(p), TASK_MIGRATE,
			 wallclock, 0);

	update_task_cpu_cycles(p, new_cpu);

	new_task = is_new_task(p);
	/* Protected by rq_lock */
	grp = p->grp;
	if (grp && sched_freq_aggregate) {
		struct group_cpu_time *cpu_time;

		migrate_type = GROUP_TO_GROUP;
		/* Protected by rq_lock */
		cpu_time = _group_cpu_time(grp, cpu_of(src_rq));
		d.src_rq = NULL;
		d.src_cpu_time = cpu_time;
		src_curr_runnable_sum = &cpu_time->curr_runnable_sum;
		src_prev_runnable_sum = &cpu_time->prev_runnable_sum;
		src_nt_curr_runnable_sum = &cpu_time->nt_curr_runnable_sum;
		src_nt_prev_runnable_sum = &cpu_time->nt_prev_runnable_sum;

		/* Protected by rq_lock */
		cpu_time = _group_cpu_time(grp, cpu_of(dest_rq));
		d.dst_rq = NULL;
		d.dst_cpu_time = cpu_time;
		dst_curr_runnable_sum = &cpu_time->curr_runnable_sum;
		dst_prev_runnable_sum = &cpu_time->prev_runnable_sum;
		dst_nt_curr_runnable_sum = &cpu_time->nt_curr_runnable_sum;
		dst_nt_prev_runnable_sum = &cpu_time->nt_prev_runnable_sum;
		sync_window_start(dest_rq, cpu_time);
	} else {
		migrate_type = RQ_TO_RQ;
		d.src_rq = src_rq;
		d.src_cpu_time = NULL;
		d.dst_rq = dest_rq;
		d.dst_cpu_time = NULL;
		src_curr_runnable_sum = &src_rq->curr_runnable_sum;
		src_prev_runnable_sum = &src_rq->prev_runnable_sum;
		src_nt_curr_runnable_sum = &src_rq->nt_curr_runnable_sum;
		src_nt_prev_runnable_sum = &src_rq->nt_prev_runnable_sum;

		dst_curr_runnable_sum = &dest_rq->curr_runnable_sum;
		dst_prev_runnable_sum = &dest_rq->prev_runnable_sum;
		dst_nt_curr_runnable_sum = &dest_rq->nt_curr_runnable_sum;
		dst_nt_prev_runnable_sum = &dest_rq->nt_prev_runnable_sum;
	}

	if (p->ravg.curr_window) {
		*src_curr_runnable_sum -= p->ravg.curr_window;
		*dst_curr_runnable_sum += p->ravg.curr_window;
		if (new_task) {
			*src_nt_curr_runnable_sum -= p->ravg.curr_window;
			*dst_nt_curr_runnable_sum += p->ravg.curr_window;
		}
	}

	if (p->ravg.prev_window) {
		*src_prev_runnable_sum -= p->ravg.prev_window;
		*dst_prev_runnable_sum += p->ravg.prev_window;
		if (new_task) {
			*src_nt_prev_runnable_sum -= p->ravg.prev_window;
			*dst_nt_prev_runnable_sum += p->ravg.prev_window;
		}
	}

	if (p == src_rq->ed_task) {
		src_rq->ed_task = NULL;
		if (!dest_rq->ed_task)
			dest_rq->ed_task = p;
	}

	trace_sched_migration_update_sum(p, migrate_type, &d);
	BUG_ON((s64)*src_prev_runnable_sum < 0);
	BUG_ON((s64)*src_curr_runnable_sum < 0);
	BUG_ON((s64)*src_nt_prev_runnable_sum < 0);
	BUG_ON((s64)*src_nt_curr_runnable_sum < 0);

done:
	if (p->state == TASK_WAKING)
		double_rq_unlock(src_rq, dest_rq);
}

#else

static inline void fixup_busy_time(struct task_struct *p, int new_cpu) { }

#endif	/* CONFIG_SCHED_FREQ_INPUT */

#define sched_up_down_migrate_auto_update 1
static void check_for_up_down_migrate_update(const struct cpumask *cpus)
{
	int i = cpumask_first(cpus);

	if (!sched_up_down_migrate_auto_update)
		return;

	if (cpu_max_possible_capacity(i) == max_possible_capacity)
		return;

	if (cpu_max_possible_freq(i) == cpu_max_freq(i))
		up_down_migrate_scale_factor = 1024;
	else
		up_down_migrate_scale_factor = (1024 *
				 cpu_max_possible_freq(i)) / cpu_max_freq(i);

	update_up_down_migrate();
}

/* Return cluster which can offer required capacity for group */
static struct sched_cluster *
best_cluster(struct related_thread_group *grp, u64 total_demand)
{
	struct sched_cluster *cluster = NULL;

	for_each_sched_cluster(cluster) {
		if (group_will_fit(cluster, grp, total_demand))
			return cluster;
	}

	return NULL;
}

static void _set_preferred_cluster(struct related_thread_group *grp)
{
	struct task_struct *p;
	u64 combined_demand = 0;

	if (!sysctl_sched_enable_colocation) {
		grp->last_update = sched_ktime_clock();
		grp->preferred_cluster = NULL;
		return;
	}

	/*
	 * wakeup of two or more related tasks could race with each other and
	 * could result in multiple calls to _set_preferred_cluster being issued
	 * at same time. Avoid overhead in such cases of rechecking preferred
	 * cluster
	 */
	if (sched_ktime_clock() - grp->last_update < sched_ravg_window / 10)
		return;

	list_for_each_entry(p, &grp->tasks, grp_list)
		combined_demand += p->ravg.demand;

	grp->preferred_cluster = best_cluster(grp, combined_demand);
	grp->last_update = sched_ktime_clock();
	trace_sched_set_preferred_cluster(grp, combined_demand);
}

static void set_preferred_cluster(struct related_thread_group *grp)
{
	raw_spin_lock(&grp->lock);
	_set_preferred_cluster(grp);
	raw_spin_unlock(&grp->lock);
}

#define ADD_TASK	0
#define REM_TASK	1

#ifdef CONFIG_SCHED_FREQ_INPUT

static void
update_task_ravg(struct task_struct *p, struct rq *rq,
		 int event, u64 wallclock, u64 irqtime);

static inline void free_group_cputime(struct related_thread_group *grp)
{
	free_percpu(grp->cpu_time);
}

static int alloc_group_cputime(struct related_thread_group *grp)
{
	int i;
	struct group_cpu_time *cpu_time;
	int cpu = raw_smp_processor_id();
	struct rq *rq = cpu_rq(cpu);
	u64 window_start = rq->window_start;

	grp->cpu_time = alloc_percpu(struct group_cpu_time);
	if (!grp->cpu_time)
		return -ENOMEM;

	for_each_possible_cpu(i) {
		cpu_time = per_cpu_ptr(grp->cpu_time, i);
		memset(cpu_time, 0, sizeof(struct group_cpu_time));
		cpu_time->window_start = window_start;
	}

	return 0;
}

/*
 * A group's window_start may be behind. When moving it forward, flip prev/curr
 * counters. When moving forward > 1 window, prev counter is set to 0
 */
static inline void
sync_window_start(struct rq *rq, struct group_cpu_time *cpu_time)
{
	u64 delta;
	int nr_windows;
	u64 curr_sum = cpu_time->curr_runnable_sum;
	u64 nt_curr_sum = cpu_time->nt_curr_runnable_sum;

	delta = rq->window_start - cpu_time->window_start;
	if (!delta)
		return;

	nr_windows = div64_u64(delta, sched_ravg_window);
	if (nr_windows > 1)
		curr_sum = nt_curr_sum = 0;

	cpu_time->prev_runnable_sum  = curr_sum;
	cpu_time->curr_runnable_sum  = 0;

	cpu_time->nt_prev_runnable_sum = nt_curr_sum;
	cpu_time->nt_curr_runnable_sum = 0;

	cpu_time->window_start = rq->window_start;
}

/*
 * Task's cpu usage is accounted in:
 *	rq->curr/prev_runnable_sum,  when its ->grp is NULL
 *	grp->cpu_time[cpu]->curr/prev_runnable_sum, when its ->grp is !NULL
 *
 * Transfer task's cpu usage between those counters when transitioning between
 * groups
 */
static void transfer_busy_time(struct rq *rq, struct related_thread_group *grp,
				struct task_struct *p, int event)
{
	u64 wallclock;
	struct group_cpu_time *cpu_time;
	u64 *src_curr_runnable_sum, *dst_curr_runnable_sum;
	u64 *src_prev_runnable_sum, *dst_prev_runnable_sum;
	u64 *src_nt_curr_runnable_sum, *dst_nt_curr_runnable_sum;
	u64 *src_nt_prev_runnable_sum, *dst_nt_prev_runnable_sum;
	struct migration_sum_data d;
	int migrate_type;

	if (!sched_freq_aggregate)
		return;

	wallclock = sched_ktime_clock();

	update_task_ravg(rq->curr, rq, TASK_UPDATE, wallclock, 0);
	update_task_ravg(p, rq, TASK_UPDATE, wallclock, 0);

	/* cpu_time protected by related_thread_group_lock, grp->lock rq_lock */
	cpu_time = _group_cpu_time(grp, cpu_of(rq));
	if (event == ADD_TASK) {
		sync_window_start(rq, cpu_time);
		migrate_type = RQ_TO_GROUP;
		d.src_rq = rq;
		d.src_cpu_time = NULL;
		d.dst_rq = NULL;
		d.dst_cpu_time = cpu_time;
		src_curr_runnable_sum = &rq->curr_runnable_sum;
		dst_curr_runnable_sum = &cpu_time->curr_runnable_sum;
		src_prev_runnable_sum = &rq->prev_runnable_sum;
		dst_prev_runnable_sum = &cpu_time->prev_runnable_sum;

		src_nt_curr_runnable_sum = &rq->nt_curr_runnable_sum;
		dst_nt_curr_runnable_sum = &cpu_time->nt_curr_runnable_sum;
		src_nt_prev_runnable_sum = &rq->nt_prev_runnable_sum;
		dst_nt_prev_runnable_sum = &cpu_time->nt_prev_runnable_sum;
	} else if (event == REM_TASK) {
		migrate_type = GROUP_TO_RQ;
		d.src_rq = NULL;
		d.src_cpu_time = cpu_time;
		d.dst_rq = rq;
		d.dst_cpu_time = NULL;

		/*
		 * In case of REM_TASK, cpu_time->window_start would be
		 * uptodate, because of the update_task_ravg() we called
		 * above on the moving task. Hence no need for
		 * sync_window_start()
		 */
		src_curr_runnable_sum = &cpu_time->curr_runnable_sum;
		dst_curr_runnable_sum = &rq->curr_runnable_sum;
		src_prev_runnable_sum = &cpu_time->prev_runnable_sum;
		dst_prev_runnable_sum = &rq->prev_runnable_sum;

		src_nt_curr_runnable_sum = &cpu_time->nt_curr_runnable_sum;
		dst_nt_curr_runnable_sum = &rq->nt_curr_runnable_sum;
		src_nt_prev_runnable_sum = &cpu_time->nt_prev_runnable_sum;
		dst_nt_prev_runnable_sum = &rq->nt_prev_runnable_sum;
	}

	*src_curr_runnable_sum -= p->ravg.curr_window;
	*dst_curr_runnable_sum += p->ravg.curr_window;

	*src_prev_runnable_sum -= p->ravg.prev_window;
	*dst_prev_runnable_sum += p->ravg.prev_window;

	if (is_new_task(p)) {
		*src_nt_curr_runnable_sum -= p->ravg.curr_window;
		*dst_nt_curr_runnable_sum += p->ravg.curr_window;
		*src_nt_prev_runnable_sum -= p->ravg.prev_window;
		*dst_nt_prev_runnable_sum += p->ravg.prev_window;
	}

	trace_sched_migration_update_sum(p, migrate_type, &d);

	BUG_ON((s64)*src_curr_runnable_sum < 0);
	BUG_ON((s64)*src_prev_runnable_sum < 0);
}

static inline struct group_cpu_time *
task_group_cpu_time(struct task_struct *p, int cpu)
{
	return _group_cpu_time(rcu_dereference(p->grp), cpu);
}

static inline struct group_cpu_time *
_group_cpu_time(struct related_thread_group *grp, int cpu)
{
	return grp ? per_cpu_ptr(grp->cpu_time, cpu) : NULL;
}

/* must be called with all rq lock held */
static inline void _reset_all_group_time(u64 window_start)
{
	struct related_thread_group *grp;

	list_for_each_entry(grp, &related_thread_groups, list) {
		int j;

		for_each_possible_cpu(j) {
			struct group_cpu_time *cpu_time;

			/* Protected by rq lock */
			cpu_time = _group_cpu_time(grp, j);
			memset(cpu_time, 0, sizeof(struct group_cpu_time));
			if (window_start)
				cpu_time->window_start = window_start;
		}
	}
}

#else	/* CONFIG_SCHED_FREQ_INPUT */

static inline void free_group_cputime(struct related_thread_group *grp) { }

static inline int alloc_group_cputime(struct related_thread_group *grp)
{
	return 0;
}

static inline void transfer_busy_time(struct rq *rq,
	 struct related_thread_group *grp, struct task_struct *p, int event)
{
}

static inline struct group_cpu_time *
task_group_cpu_time(struct task_struct *p, int cpu)
{
	return NULL;
}

static inline struct group_cpu_time *
_group_cpu_time(struct related_thread_group *grp, int cpu)
{
	return NULL;
}

static inline void _reset_all_group_time(u64 window_start) {}

#endif

struct related_thread_group *alloc_related_thread_group(int group_id)
{
	struct related_thread_group *grp;

	grp = kzalloc(sizeof(*grp), GFP_KERNEL);
	if (!grp)
		return ERR_PTR(-ENOMEM);

	if (alloc_group_cputime(grp)) {
		kfree(grp);
		return ERR_PTR(-ENOMEM);
	}

	grp->id = group_id;
	INIT_LIST_HEAD(&grp->tasks);
	INIT_LIST_HEAD(&grp->list);
	raw_spin_lock_init(&grp->lock);

	return grp;
}

struct related_thread_group *lookup_related_thread_group(unsigned int group_id)
{
	struct related_thread_group *grp;

	list_for_each_entry(grp, &related_thread_groups, list) {
		if (grp->id == group_id)
			return grp;
	}

	return NULL;
}

/* See comments before preferred_cluster() */
static void free_related_thread_group(struct rcu_head *rcu)
{
	struct related_thread_group *grp = container_of(rcu, struct
			related_thread_group, rcu);

	free_group_cputime(grp);
	kfree(grp);
}

static void remove_task_from_group(struct task_struct *p)
{
	struct related_thread_group *grp = p->grp;
	struct rq *rq;
	int empty_group = 1;

	raw_spin_lock(&grp->lock);

	rq = __task_rq_lock(p);
	transfer_busy_time(rq, p->grp, p, REM_TASK);
	list_del_init(&p->grp_list);
	rcu_assign_pointer(p->grp, NULL);
	__task_rq_unlock(rq);

	if (!list_empty(&grp->tasks)) {
		empty_group = 0;
		_set_preferred_cluster(grp);
	}

	raw_spin_unlock(&grp->lock);

	if (empty_group) {
		list_del(&grp->list);
		/*
		 * RCU, kswapd etc tasks can get woken up from
		 * call_rcu(). As the wakeup path also acquires
		 * the related_thread_group_lock, drop it here.
		 */
		write_unlock(&related_thread_group_lock);
		call_rcu(&grp->rcu, free_related_thread_group);
		write_lock(&related_thread_group_lock);
	}
}

static int
add_task_to_group(struct task_struct *p, struct related_thread_group *grp)
{
	struct rq *rq;

	raw_spin_lock(&grp->lock);

	/*
	 * Change p->grp under rq->lock. Will prevent races with read-side
	 * reference of p->grp in various hot-paths
	 */
	rq = __task_rq_lock(p);
	transfer_busy_time(rq, grp, p, ADD_TASK);
	list_add(&p->grp_list, &grp->tasks);
	rcu_assign_pointer(p->grp, grp);
	__task_rq_unlock(rq);

	_set_preferred_cluster(grp);

	raw_spin_unlock(&grp->lock);

	return 0;
}

static void add_new_task_to_grp(struct task_struct *new)
{
	unsigned long flags;
	struct related_thread_group *grp;
	struct task_struct *parent;

	if (!sysctl_sched_enable_thread_grouping)
		return;

	if (thread_group_leader(new))
		return;

	parent = new->group_leader;

	/*
	 * The parent's pi_lock is required here to protect race
	 * against the parent task being removed from the
	 * group.
	 */
	raw_spin_lock_irqsave(&parent->pi_lock, flags);

	/* protected by pi_lock. */
	grp = task_related_thread_group(parent);
	if (!grp) {
		raw_spin_unlock_irqrestore(&parent->pi_lock, flags);
		return;
	}
	raw_spin_lock(&grp->lock);
	raw_spin_unlock_irqrestore(&parent->pi_lock, flags);

	rcu_assign_pointer(new->grp, grp);
	list_add(&new->grp_list, &grp->tasks);

	raw_spin_unlock(&grp->lock);
}

int sched_set_group_id(struct task_struct *p, unsigned int group_id)
{
	int rc = 0, destroy = 0;
	unsigned long flags;
	struct related_thread_group *grp = NULL, *new = NULL;

redo:
	raw_spin_lock_irqsave(&p->pi_lock, flags);

	if ((current != p && p->flags & PF_EXITING) ||
			(!p->grp && !group_id) ||
			(p->grp && p->grp->id == group_id))
		goto done;

	write_lock(&related_thread_group_lock);

	if (!group_id) {
		remove_task_from_group(p);
		write_unlock(&related_thread_group_lock);
		goto done;
	}

	if (p->grp && p->grp->id != group_id)
		remove_task_from_group(p);

	grp = lookup_related_thread_group(group_id);
	if (!grp && !new) {
		/* New group */
		write_unlock(&related_thread_group_lock);
		raw_spin_unlock_irqrestore(&p->pi_lock, flags);
		new = alloc_related_thread_group(group_id);
		if (IS_ERR(new))
			return -ENOMEM;
		destroy = 1;
		/* Rerun checks (like task exiting), since we dropped pi_lock */
		goto redo;
	} else if (!grp && new) {
		/* New group - use object allocated before */
		destroy = 0;
		list_add(&new->list, &related_thread_groups);
		grp = new;
	}

	BUG_ON(!grp);
	rc = add_task_to_group(p, grp);
	write_unlock(&related_thread_group_lock);
done:
	raw_spin_unlock_irqrestore(&p->pi_lock, flags);

	if (new && destroy) {
		free_group_cputime(new);
		kfree(new);
	}

	return rc;
}

unsigned int sched_get_group_id(struct task_struct *p)
{
	unsigned int group_id;
	struct related_thread_group *grp;

	rcu_read_lock();
	grp = task_related_thread_group(p);
	group_id = grp ? grp->id : 0;
	rcu_read_unlock();

	return group_id;
}

static void update_cpu_cluster_capacity(const cpumask_t *cpus)
{
	int i;
	struct sched_cluster *cluster;
	struct cpumask cpumask;

	cpumask_copy(&cpumask, cpus);
	pre_big_task_count_change(cpu_possible_mask);

	for_each_cpu(i, &cpumask) {
		cluster = cpu_rq(i)->cluster;
		cpumask_andnot(&cpumask, &cpumask, &cluster->cpus);

		cluster->capacity = compute_capacity(cluster);
		cluster->load_scale_factor = compute_load_scale_factor(cluster);

		/* 'cpus' can contain cpumask more than one cluster */
		check_for_up_down_migrate_update(&cluster->cpus);
	}

	__update_min_max_capacity();

	post_big_task_count_change(cpu_possible_mask);
}

void sched_update_cpu_freq_min_max(const cpumask_t *cpus, u32 fmin, u32 fmax)
{
	struct cpumask cpumask;
	struct sched_cluster *cluster;
	unsigned int orig_max_freq;
	int i, update_capacity = 0;

	cpumask_copy(&cpumask, cpus);
	for_each_cpu(i, &cpumask) {
		cluster = cpu_rq(i)->cluster;
		cpumask_andnot(&cpumask, &cpumask, &cluster->cpus);

		orig_max_freq = cpu_max_freq(i);
		cluster->max_mitigated_freq = fmax;

		update_capacity += (orig_max_freq != cpu_max_freq(i));
	}

	if (update_capacity)
		update_cpu_cluster_capacity(cpus);
}

static int cpufreq_notifier_policy(struct notifier_block *nb,
		unsigned long val, void *data)
{
	struct cpufreq_policy *policy = (struct cpufreq_policy *)data;
	struct sched_cluster *cluster = NULL;
	struct cpumask policy_cluster = *policy->related_cpus;
	unsigned int orig_max_freq = 0;
	int i, j, update_capacity = 0;

	if (val != CPUFREQ_NOTIFY && val != CPUFREQ_REMOVE_POLICY &&
						val != CPUFREQ_CREATE_POLICY)
		return 0;

	if (val == CPUFREQ_REMOVE_POLICY || val == CPUFREQ_CREATE_POLICY) {
		update_min_max_capacity();
		return 0;
	}

	max_possible_freq = max(max_possible_freq, policy->cpuinfo.max_freq);
	if (min_max_freq == 1)
		min_max_freq = UINT_MAX;
	min_max_freq = min(min_max_freq, policy->cpuinfo.max_freq);
	BUG_ON(!min_max_freq);
	BUG_ON(!policy->max);

	for_each_cpu(i, &policy_cluster) {
		cluster = cpu_rq(i)->cluster;
		cpumask_andnot(&policy_cluster, &policy_cluster,
						&cluster->cpus);

		orig_max_freq = cpu_max_freq(i);
		cluster->min_freq = policy->min;
		cluster->max_freq = policy->max;
		cluster->cur_freq = policy->cur;

		if (!cluster->freq_init_done) {
			mutex_lock(&cluster_lock);
			for_each_cpu(j, &cluster->cpus)
				cpumask_copy(&cpu_rq(j)->freq_domain_cpumask,
						policy->related_cpus);
			cluster->max_possible_freq = policy->cpuinfo.max_freq;
			cluster->max_possible_capacity =
				compute_max_possible_capacity(cluster);
			cluster->freq_init_done = true;

			sort_clusters();
			update_all_clusters_stats();
			mutex_unlock(&cluster_lock);
			continue;
		}

		update_capacity += (orig_max_freq != cpu_max_freq(i));
	}

	if (update_capacity)
		update_cpu_cluster_capacity(policy->related_cpus);

	return 0;
}

static int cpufreq_notifier_trans(struct notifier_block *nb,
		unsigned long val, void *data)
{
	struct cpufreq_freqs *freq = (struct cpufreq_freqs *)data;
	unsigned int cpu = freq->cpu, new_freq = freq->new;
	unsigned long flags;
	struct sched_cluster *cluster;
	struct cpumask policy_cpus = cpu_rq(cpu)->freq_domain_cpumask;
	int i, j;

	if (val != CPUFREQ_POSTCHANGE)
		return 0;

	BUG_ON(!new_freq);

	if (cpu_cur_freq(cpu) == new_freq)
		return 0;

	for_each_cpu(i, &policy_cpus) {
		cluster = cpu_rq(i)->cluster;

		for_each_cpu(j, &cluster->cpus) {
			struct rq *rq = cpu_rq(j);

			raw_spin_lock_irqsave(&rq->lock, flags);
			update_task_ravg(rq->curr, rq, TASK_UPDATE,
						sched_ktime_clock(), 0);
			raw_spin_unlock_irqrestore(&rq->lock, flags);
		}

		cluster->cur_freq = new_freq;
		cpumask_andnot(&policy_cpus, &policy_cpus, &cluster->cpus);
	}

	return 0;
}

static int pwr_stats_ready_notifier(struct notifier_block *nb,
				    unsigned long cpu, void *data)
{
	cpumask_t mask = CPU_MASK_NONE;

	cpumask_set_cpu(cpu, &mask);
	sched_update_freq_max_load(&mask);

	mutex_lock(&cluster_lock);
	sort_clusters();
	mutex_unlock(&cluster_lock);

	return 0;
}

static struct notifier_block notifier_policy_block = {
	.notifier_call = cpufreq_notifier_policy
};

static struct notifier_block notifier_trans_block = {
	.notifier_call = cpufreq_notifier_trans
};

static struct notifier_block notifier_pwr_stats_ready = {
	.notifier_call = pwr_stats_ready_notifier
};

int __weak register_cpu_pwr_stats_ready_notifier(struct notifier_block *nb)
{
	return -EINVAL;
}

static int register_sched_callback(void)
{
	int ret;

	if (!sched_enable_hmp)
		return 0;

	ret = cpufreq_register_notifier(&notifier_policy_block,
						CPUFREQ_POLICY_NOTIFIER);

	if (!ret)
		ret = cpufreq_register_notifier(&notifier_trans_block,
						CPUFREQ_TRANSITION_NOTIFIER);

	register_cpu_pwr_stats_ready_notifier(&notifier_pwr_stats_ready);

	return 0;
}

/*
 * cpufreq callbacks can be registered at core_initcall or later time.
 * Any registration done prior to that is "forgotten" by cpufreq. See
 * initialization of variable init_cpufreq_transition_notifier_list_called
 * for further information.
 */
core_initcall(register_sched_callback);

static inline int update_preferred_cluster(struct related_thread_group *grp,
		struct task_struct *p, u32 old_load)
{
	u32 new_load = task_load(p);

	if (!grp)
		return 0;

	/*
	 * Update if task's load has changed significantly or a complete window
	 * has passed since we last updated preference
	 */
	if (abs(new_load - old_load) > sched_ravg_window / 4 ||
		sched_ktime_clock() - grp->last_update > sched_ravg_window)
		return 1;

	return 0;
}

#else	/* CONFIG_SCHED_HMP */

static inline void fixup_busy_time(struct task_struct *p, int new_cpu) { }

static void
update_task_ravg(struct task_struct *p, struct rq *rq,
			 int event, u64 wallclock, u64 irqtime)
{
}

static inline void mark_task_starting(struct task_struct *p) {}

static inline void set_window_start(struct rq *rq) {}

static inline void migrate_sync_cpu(int cpu) {}

#endif	/* CONFIG_SCHED_HMP */

#ifdef CONFIG_SMP
void set_task_cpu(struct task_struct *p, unsigned int new_cpu)
{
#ifdef CONFIG_SCHED_DEBUG
	/*
	 * We should never call set_task_cpu() on a blocked task,
	 * ttwu() will sort out the placement.
	 */
	WARN_ON_ONCE(p->state != TASK_RUNNING && p->state != TASK_WAKING &&
			!(task_preempt_count(p) & PREEMPT_ACTIVE));

#ifdef CONFIG_LOCKDEP
	/*
	 * The caller should hold either p->pi_lock or rq->lock, when changing
	 * a task's CPU. ->pi_lock for waking tasks, rq->lock for runnable tasks.
	 *
	 * sched_move_task() holds both and thus holding either pins the cgroup,
	 * see task_group().
	 *
	 * Furthermore, all task_rq users should acquire both locks, see
	 * task_rq_lock().
	 */
	WARN_ON_ONCE(debug_locks && !(lockdep_is_held(&p->pi_lock) ||
				      lockdep_is_held(&task_rq(p)->lock)));
#endif
#endif

	trace_sched_migrate_task(p, new_cpu);

	if (task_cpu(p) != new_cpu) {
		if (p->sched_class->migrate_task_rq)
			p->sched_class->migrate_task_rq(p, new_cpu);
		p->se.nr_migrations++;
		perf_sw_event(PERF_COUNT_SW_CPU_MIGRATIONS, 1, NULL, 0);

		fixup_busy_time(p, new_cpu);
	}

	__set_task_cpu(p, new_cpu);
}

static void __migrate_swap_task(struct task_struct *p, int cpu)
{
	if (task_on_rq_queued(p)) {
		struct rq *src_rq, *dst_rq;

		src_rq = task_rq(p);
		dst_rq = cpu_rq(cpu);

		deactivate_task(src_rq, p, 0);
		p->on_rq = TASK_ON_RQ_MIGRATING;
		set_task_cpu(p, cpu);
		p->on_rq = TASK_ON_RQ_QUEUED;
		activate_task(dst_rq, p, 0);
		check_preempt_curr(dst_rq, p, 0);
	} else {
		/*
		 * Task isn't running anymore; make it appear like we migrated
		 * it before it went to sleep. This means on wakeup we make the
		 * previous cpu our targer instead of where it really is.
		 */
		p->wake_cpu = cpu;
	}
}

struct migration_swap_arg {
	struct task_struct *src_task, *dst_task;
	int src_cpu, dst_cpu;
};

static int migrate_swap_stop(void *data)
{
	struct migration_swap_arg *arg = data;
	struct rq *src_rq, *dst_rq;
	int ret = -EAGAIN;

	src_rq = cpu_rq(arg->src_cpu);
	dst_rq = cpu_rq(arg->dst_cpu);

	double_raw_lock(&arg->src_task->pi_lock,
			&arg->dst_task->pi_lock);
	double_rq_lock(src_rq, dst_rq);
	if (task_cpu(arg->dst_task) != arg->dst_cpu)
		goto unlock;

	if (task_cpu(arg->src_task) != arg->src_cpu)
		goto unlock;

	if (!cpumask_test_cpu(arg->dst_cpu, tsk_cpus_allowed(arg->src_task)))
		goto unlock;

	if (!cpumask_test_cpu(arg->src_cpu, tsk_cpus_allowed(arg->dst_task)))
		goto unlock;

	__migrate_swap_task(arg->src_task, arg->dst_cpu);
	__migrate_swap_task(arg->dst_task, arg->src_cpu);

	ret = 0;

unlock:
	double_rq_unlock(src_rq, dst_rq);
	raw_spin_unlock(&arg->dst_task->pi_lock);
	raw_spin_unlock(&arg->src_task->pi_lock);

	return ret;
}

/*
 * Cross migrate two tasks
 */
int migrate_swap(struct task_struct *cur, struct task_struct *p)
{
	struct migration_swap_arg arg;
	int ret = -EINVAL;

	arg = (struct migration_swap_arg){
		.src_task = cur,
		.src_cpu = task_cpu(cur),
		.dst_task = p,
		.dst_cpu = task_cpu(p),
	};

	if (arg.src_cpu == arg.dst_cpu)
		goto out;

	/*
	 * These three tests are all lockless; this is OK since all of them
	 * will be re-checked with proper locks held further down the line.
	 */
	if (!cpu_active(arg.src_cpu) || !cpu_active(arg.dst_cpu))
		goto out;

	if (!cpumask_test_cpu(arg.dst_cpu, tsk_cpus_allowed(arg.src_task)))
		goto out;

	if (!cpumask_test_cpu(arg.src_cpu, tsk_cpus_allowed(arg.dst_task)))
		goto out;

	trace_sched_swap_numa(cur, arg.src_cpu, p, arg.dst_cpu);
	ret = stop_two_cpus(arg.dst_cpu, arg.src_cpu, migrate_swap_stop, &arg);

out:
	return ret;
}

struct migration_arg {
	struct task_struct *task;
	int dest_cpu;
};

static int migration_cpu_stop(void *data);

/*
 * wait_task_inactive - wait for a thread to unschedule.
 *
 * If @match_state is nonzero, it's the @p->state value just checked and
 * not expected to change.  If it changes, i.e. @p might have woken up,
 * then return zero.  When we succeed in waiting for @p to be off its CPU,
 * we return a positive number (its total switch count).  If a second call
 * a short while later returns the same number, the caller can be sure that
 * @p has remained unscheduled the whole time.
 *
 * The caller must ensure that the task *will* unschedule sometime soon,
 * else this function might spin for a *long* time. This function can't
 * be called with interrupts off, or it may introduce deadlock with
 * smp_call_function() if an IPI is sent by the same process we are
 * waiting to become inactive.
 */
unsigned long wait_task_inactive(struct task_struct *p, long match_state)
{
	unsigned long flags;
	int running, queued;
	unsigned long ncsw;
	struct rq *rq;

	for (;;) {
		/*
		 * We do the initial early heuristics without holding
		 * any task-queue locks at all. We'll only try to get
		 * the runqueue lock when things look like they will
		 * work out!
		 */
		rq = task_rq(p);

		/*
		 * If the task is actively running on another CPU
		 * still, just relax and busy-wait without holding
		 * any locks.
		 *
		 * NOTE! Since we don't hold any locks, it's not
		 * even sure that "rq" stays as the right runqueue!
		 * But we don't care, since "task_running()" will
		 * return false if the runqueue has changed and p
		 * is actually now running somewhere else!
		 */
		while (task_running(rq, p)) {
			if (match_state && unlikely(p->state != match_state))
				return 0;
			cpu_relax();
		}

		/*
		 * Ok, time to look more closely! We need the rq
		 * lock now, to be *sure*. If we're wrong, we'll
		 * just go back and repeat.
		 */
		rq = task_rq_lock(p, &flags);
		trace_sched_wait_task(p);
		running = task_running(rq, p);
		queued = task_on_rq_queued(p);
		ncsw = 0;
		if (!match_state || p->state == match_state)
			ncsw = p->nvcsw | LONG_MIN; /* sets MSB */
		task_rq_unlock(rq, p, &flags);

		/*
		 * If it changed from the expected state, bail out now.
		 */
		if (unlikely(!ncsw))
			break;

		/*
		 * Was it really running after all now that we
		 * checked with the proper locks actually held?
		 *
		 * Oops. Go back and try again..
		 */
		if (unlikely(running)) {
			cpu_relax();
			continue;
		}

		/*
		 * It's not enough that it's not actively running,
		 * it must be off the runqueue _entirely_, and not
		 * preempted!
		 *
		 * So if it was still runnable (but just not actively
		 * running right now), it's preempted, and we should
		 * yield - it could be a while.
		 */
		if (unlikely(queued)) {
			ktime_t to = ktime_set(0, NSEC_PER_MSEC);

			set_current_state(TASK_UNINTERRUPTIBLE);
			schedule_hrtimeout(&to, HRTIMER_MODE_REL);
			continue;
		}

		/*
		 * Ahh, all good. It wasn't running, and it wasn't
		 * runnable, which means that it will never become
		 * running in the future either. We're all done!
		 */
		break;
	}

	return ncsw;
}

/***
 * kick_process - kick a running thread to enter/exit the kernel
 * @p: the to-be-kicked thread
 *
 * Cause a process which is running on another CPU to enter
 * kernel-mode, without any delay. (to get signals handled.)
 *
 * NOTE: this function doesn't have to take the runqueue lock,
 * because all it wants to ensure is that the remote task enters
 * the kernel. If the IPI races and the task has been migrated
 * to another CPU then no harm is done and the purpose has been
 * achieved as well.
 */
void kick_process(struct task_struct *p)
{
	int cpu;

	preempt_disable();
	cpu = task_cpu(p);
	if ((cpu != smp_processor_id()) && task_curr(p))
		smp_send_reschedule(cpu);
	preempt_enable();
}
EXPORT_SYMBOL_GPL(kick_process);
#endif /* CONFIG_SMP */

#ifdef CONFIG_SMP
/*
 * ->cpus_allowed is protected by both rq->lock and p->pi_lock
 */
static int select_fallback_rq(int cpu, struct task_struct *p)
{
	int nid = cpu_to_node(cpu);
	const struct cpumask *nodemask = NULL;
	enum { cpuset, possible, fail } state = cpuset;
	int dest_cpu;

	/*
	 * If the node that the cpu is on has been offlined, cpu_to_node()
	 * will return -1. There is no cpu on the node, and we should
	 * select the cpu on the other node.
	 */
	if (nid != -1) {
		nodemask = cpumask_of_node(nid);

		/* Look for allowed, online CPU in same node. */
		for_each_cpu(dest_cpu, nodemask) {
			if (!cpu_online(dest_cpu))
				continue;
			if (!cpu_active(dest_cpu))
				continue;
			if (cpumask_test_cpu(dest_cpu, tsk_cpus_allowed(p)))
				return dest_cpu;
		}
	}

	for (;;) {
		/* Any allowed, online CPU? */
		for_each_cpu(dest_cpu, tsk_cpus_allowed(p)) {
			if (!cpu_online(dest_cpu))
				continue;
			if (!cpu_active(dest_cpu))
				continue;
			goto out;
		}

		switch (state) {
		case cpuset:
			/* No more Mr. Nice Guy. */
			cpuset_cpus_allowed_fallback(p);
			state = possible;
			break;

		case possible:
			do_set_cpus_allowed(p, cpu_possible_mask);
			state = fail;
			break;

		case fail:
			BUG();
			break;
		}
	}

out:
	if (state != cpuset) {
		/*
		 * Don't tell them about moving exiting tasks or
		 * kernel threads (both mm NULL), since they never
		 * leave kernel.
		 */
		if (p->mm && printk_ratelimit()) {
			printk_deferred("process %d (%s) no longer affine to cpu%d\n",
					task_pid_nr(p), p->comm, cpu);
		}
	}

	return dest_cpu;
}

/*
 * The caller (fork, wakeup) owns p->pi_lock, ->cpus_allowed is stable.
 */
static inline
int select_task_rq(struct task_struct *p, int cpu, int sd_flags, int wake_flags)
{
	cpu = p->sched_class->select_task_rq(p, cpu, sd_flags, wake_flags);

	/*
	 * In order not to call set_task_cpu() on a blocking task we need
	 * to rely on ttwu() to place the task on a valid ->cpus_allowed
	 * cpu.
	 *
	 * Since this is common to all placement strategies, this lives here.
	 *
	 * [ this allows ->select_task() to simply return task_cpu(p) and
	 *   not worry about this generic constraint ]
	 */
	if (unlikely(!cpumask_test_cpu(cpu, tsk_cpus_allowed(p)) ||
		     !cpu_online(cpu)))
		cpu = select_fallback_rq(task_cpu(p), p);

	return cpu;
}

static void update_avg(u64 *avg, u64 sample)
{
	s64 diff = sample - *avg;
	*avg += diff >> 3;
}
#endif

static void
ttwu_stat(struct task_struct *p, int cpu, int wake_flags)
{
#ifdef CONFIG_SCHEDSTATS
	struct rq *rq = this_rq();

#ifdef CONFIG_SMP
	int this_cpu = smp_processor_id();

	if (cpu == this_cpu) {
		schedstat_inc(rq, ttwu_local);
		schedstat_inc(p, se.statistics.nr_wakeups_local);
	} else {
		struct sched_domain *sd;

		schedstat_inc(p, se.statistics.nr_wakeups_remote);
		rcu_read_lock();
		for_each_domain(this_cpu, sd) {
			if (cpumask_test_cpu(cpu, sched_domain_span(sd))) {
				schedstat_inc(sd, ttwu_wake_remote);
				break;
			}
		}
		rcu_read_unlock();
	}

	if (wake_flags & WF_MIGRATED)
		schedstat_inc(p, se.statistics.nr_wakeups_migrate);

#endif /* CONFIG_SMP */

	schedstat_inc(rq, ttwu_count);
	schedstat_inc(p, se.statistics.nr_wakeups);

	if (wake_flags & WF_SYNC)
		schedstat_inc(p, se.statistics.nr_wakeups_sync);

#endif /* CONFIG_SCHEDSTATS */
}

static inline void ttwu_activate(struct rq *rq, struct task_struct *p, int en_flags)
{
	activate_task(rq, p, en_flags);
	p->on_rq = TASK_ON_RQ_QUEUED;

	/* if a worker is waking up, notify workqueue */
	if (p->flags & PF_WQ_WORKER)
		wq_worker_waking_up(p, cpu_of(rq));
}

/*
 * Mark the task runnable and perform wakeup-preemption.
 */
static void
ttwu_do_wakeup(struct rq *rq, struct task_struct *p, int wake_flags)
{
	check_preempt_curr(rq, p, wake_flags);
	trace_sched_wakeup(p, true);

	p->state = TASK_RUNNING;
#ifdef CONFIG_SMP
	if (p->sched_class->task_woken)
		p->sched_class->task_woken(rq, p);

	if (rq->idle_stamp) {
		u64 delta = rq_clock(rq) - rq->idle_stamp;
		u64 max = 2*rq->max_idle_balance_cost;

		update_avg(&rq->avg_idle, delta);

		if (rq->avg_idle > max)
			rq->avg_idle = max;

		rq->idle_stamp = 0;
	}
#endif
}

static void
ttwu_do_activate(struct rq *rq, struct task_struct *p, int wake_flags)
{
#ifdef CONFIG_SMP
	if (p->sched_contributes_to_load)
		rq->nr_uninterruptible--;
#endif

	ttwu_activate(rq, p, ENQUEUE_WAKEUP | ENQUEUE_WAKING);
	ttwu_do_wakeup(rq, p, wake_flags);
}

/*
 * Called in case the task @p isn't fully descheduled from its runqueue,
 * in this case we must do a remote wakeup. Its a 'light' wakeup though,
 * since all we need to do is flip p->state to TASK_RUNNING, since
 * the task is still ->on_rq.
 */
static int ttwu_remote(struct task_struct *p, int wake_flags)
{
	struct rq *rq;
	int ret = 0;

	rq = __task_rq_lock(p);
	if (task_on_rq_queued(p)) {
		/* check_preempt_curr() may use rq clock */
		update_rq_clock(rq);
		ttwu_do_wakeup(rq, p, wake_flags);
		ret = 1;
	}
	__task_rq_unlock(rq);

	return ret;
}

#ifdef CONFIG_SMP
void sched_ttwu_pending(void)
{
	struct rq *rq = this_rq();
	struct llist_node *llist = llist_del_all(&rq->wake_list);
	struct task_struct *p;
	unsigned long flags;

	if (!llist)
		return;

	raw_spin_lock_irqsave(&rq->lock, flags);

	while (llist) {
		p = llist_entry(llist, struct task_struct, wake_entry);
		llist = llist_next(llist);
		ttwu_do_activate(rq, p, 0);
	}

	raw_spin_unlock_irqrestore(&rq->lock, flags);
}

void scheduler_ipi(void)
{
	int cpu = smp_processor_id();

	/*
	 * Fold TIF_NEED_RESCHED into the preempt_count; anybody setting
	 * TIF_NEED_RESCHED remotely (for the first time) will also send
	 * this IPI.
	 */
	preempt_fold_need_resched();

	if (llist_empty(&this_rq()->wake_list) && !got_nohz_idle_kick() &&
							!got_boost_kick())
		return;

	if (got_boost_kick()) {
		struct rq *rq = cpu_rq(cpu);

		if (rq->curr->sched_class == &fair_sched_class)
			check_for_migration(rq, rq->curr);
		clear_boost_kick(cpu);
	}

	/*
	 * Not all reschedule IPI handlers call irq_enter/irq_exit, since
	 * traditionally all their work was done from the interrupt return
	 * path. Now that we actually do some work, we need to make sure
	 * we do call them.
	 *
	 * Some archs already do call them, luckily irq_enter/exit nest
	 * properly.
	 *
	 * Arguably we should visit all archs and update all handlers,
	 * however a fair share of IPIs are still resched only so this would
	 * somewhat pessimize the simple resched case.
	 */
	irq_enter();
	sched_ttwu_pending();

	/*
	 * Check if someone kicked us for doing the nohz idle load balance.
	 */
	if (unlikely(got_nohz_idle_kick())) {
		this_rq()->idle_balance = 1;
		raise_softirq_irqoff(SCHED_SOFTIRQ);
	}
	irq_exit();
}

static void ttwu_queue_remote(struct task_struct *p, int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	if (llist_add(&p->wake_entry, &cpu_rq(cpu)->wake_list)) {
		if (!set_nr_if_polling(rq->idle))
			smp_send_reschedule(cpu);
		else
			trace_sched_wake_idle_without_ipi(cpu);
	}
}

void wake_up_if_idle(int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	unsigned long flags;

	rcu_read_lock();

	if (!is_idle_task(rcu_dereference(rq->curr)))
		goto out;

	if (set_nr_if_polling(rq->idle)) {
		trace_sched_wake_idle_without_ipi(cpu);
	} else {
		raw_spin_lock_irqsave(&rq->lock, flags);
		if (is_idle_task(rq->curr))
			smp_send_reschedule(cpu);
		/* Else cpu is not in idle, do nothing here */
		raw_spin_unlock_irqrestore(&rq->lock, flags);
	}

out:
	rcu_read_unlock();
}

bool cpus_share_cache(int this_cpu, int that_cpu)
{
	return per_cpu(sd_llc_id, this_cpu) == per_cpu(sd_llc_id, that_cpu);
}
#endif /* CONFIG_SMP */

static void ttwu_queue(struct task_struct *p, int cpu)
{
	struct rq *rq = cpu_rq(cpu);

#if defined(CONFIG_SMP)
	if (sched_feat(TTWU_QUEUE) && !cpus_share_cache(smp_processor_id(), cpu)) {
		sched_clock_cpu(cpu); /* sync clocks x-cpu */
		ttwu_queue_remote(p, cpu);
		return;
	}
#endif

	raw_spin_lock(&rq->lock);
	ttwu_do_activate(rq, p, 0);
	raw_spin_unlock(&rq->lock);
}

__read_mostly unsigned int sysctl_sched_wakeup_load_threshold = 110;

/**
 * try_to_wake_up - wake up a thread
 * @p: the thread to be awakened
 * @state: the mask of task states that can be woken
 * @wake_flags: wake modifier flags (WF_*)
 *
 * Put it on the run-queue if it's not already there. The "current"
 * thread is always on the run-queue (except when the actual
 * re-schedule is in progress), and as such you're allowed to do
 * the simpler "current->state = TASK_RUNNING" to mark yourself
 * runnable without the overhead of this.
 *
 * Return: %true if @p was woken up, %false if it was already running.
 * or @state didn't match @p's state.
 */
static int
try_to_wake_up(struct task_struct *p, unsigned int state, int wake_flags)
{
	unsigned long flags;
	int cpu, src_cpu, success = 0;
	int notify = 0;
	struct migration_notify_data mnd;
#ifdef CONFIG_SMP
	unsigned int old_load;
	struct rq *rq;
	u64 wallclock;
	struct related_thread_group *grp = NULL;
#endif
	bool freq_notif_allowed = !(wake_flags & WF_NO_NOTIFIER);
	bool check_group = false;

	wake_flags &= ~WF_NO_NOTIFIER;

	/*
	 * If we are going to wake up a thread waiting for CONDITION we
	 * need to ensure that CONDITION=1 done by the caller can not be
	 * reordered with p->state check below. This pairs with mb() in
	 * set_current_state() the waiting thread does.
	 */
	smp_mb__before_spinlock();
	raw_spin_lock_irqsave(&p->pi_lock, flags);
	src_cpu = cpu = task_cpu(p);

	if (!(p->state & state))
		goto out;

	success = 1; /* we're going to change ->state */

	if (p->on_rq && ttwu_remote(p, wake_flags))
		goto stat;

#ifdef CONFIG_SMP
	/*
	 * If the owning (remote) cpu is still in the middle of schedule() with
	 * this task as prev, wait until its done referencing the task.
	 */
	while (p->on_cpu)
		cpu_relax();
	/*
	 * Pairs with the smp_wmb() in finish_lock_switch().
	 */
	smp_rmb();

	rq = cpu_rq(task_cpu(p));

	raw_spin_lock(&rq->lock);
	old_load = task_load(p);
	wallclock = sched_ktime_clock();
	update_task_ravg(rq->curr, rq, TASK_UPDATE, wallclock, 0);
	update_task_ravg(p, rq, TASK_WAKE, wallclock, 0);
	raw_spin_unlock(&rq->lock);

	rcu_read_lock();
	grp = task_related_thread_group(p);
	if (update_preferred_cluster(grp, p, old_load))
		set_preferred_cluster(grp);
	rcu_read_unlock();
	check_group = grp != NULL;

	p->sched_contributes_to_load = !!task_contributes_to_load(p);
	p->state = TASK_WAKING;

	if (p->sched_class->task_waking)
		p->sched_class->task_waking(p);

	cpu = select_task_rq(p, p->wake_cpu, SD_BALANCE_WAKE, wake_flags);

	/* Refresh src_cpu as it could have changed since we last read it */
	src_cpu = task_cpu(p);
	if (src_cpu != cpu) {
		wake_flags |= WF_MIGRATED;
		set_task_cpu(p, cpu);
	}

	set_task_last_wake(p, wallclock);
#endif /* CONFIG_SMP */
	ttwu_queue(p, cpu);
stat:
	ttwu_stat(p, cpu, wake_flags);

	if (task_notify_on_migrate(p)) {
		mnd.src_cpu = src_cpu;
		mnd.dest_cpu = cpu;
		mnd.load = pct_task_load(p);

		/*
		 * Call the migration notifier with mnd for foreground task
		 * migrations as well as for wakeups if their load is above
		 * sysctl_sched_wakeup_load_threshold. This would prompt the
		 * cpu-boost to boost the CPU frequency on wake up of a heavy
		 * weight foreground task
		 */
		if ((src_cpu != cpu) || (mnd.load >
					sysctl_sched_wakeup_load_threshold))
			notify = 1;
	}

out:
	raw_spin_unlock_irqrestore(&p->pi_lock, flags);

	if (notify)
		atomic_notifier_call_chain(&migration_notifier_head,
					   0, (void *)&mnd);

	if (freq_notif_allowed) {
		if (!same_freq_domain(src_cpu, cpu)) {
			check_for_freq_change(cpu_rq(cpu),
						false, check_group);
			check_for_freq_change(cpu_rq(src_cpu),
						false, check_group);
		} else if (success) {
			check_for_freq_change(cpu_rq(cpu), true, false);
		}
	}

	return success;
}

/**
 * try_to_wake_up_local - try to wake up a local task with rq lock held
 * @p: the thread to be awakened
 *
 * Put @p on the run-queue if it's not already there. The caller must
 * ensure that this_rq() is locked, @p is bound to this_rq() and not
 * the current task.
 */
static void try_to_wake_up_local(struct task_struct *p)
{
	struct rq *rq = task_rq(p);

	if (rq != this_rq() || p == current) {
		printk_deferred("%s: Failed to wakeup task %d (%s), rq = %p,"
				" this_rq = %p, p = %p, current = %p\n",
			__func__, task_pid_nr(p), p->comm, rq,
			this_rq(), p, current);
		return;
	}

	lockdep_assert_held(&rq->lock);

	if (!raw_spin_trylock(&p->pi_lock)) {
		raw_spin_unlock(&rq->lock);
		raw_spin_lock(&p->pi_lock);
		raw_spin_lock(&rq->lock);
	}

	if (!(p->state & TASK_NORMAL))
		goto out;

	if (!task_on_rq_queued(p)) {
		u64 wallclock = sched_ktime_clock();

		update_task_ravg(rq->curr, rq, TASK_UPDATE, wallclock, 0);
		update_task_ravg(p, rq, TASK_WAKE, wallclock, 0);
		ttwu_activate(rq, p, ENQUEUE_WAKEUP);
		set_task_last_wake(p, wallclock);
	}

	ttwu_do_wakeup(rq, p, 0);
	ttwu_stat(p, smp_processor_id(), 0);
out:
	raw_spin_unlock(&p->pi_lock);
	/* Todo : Send cpufreq notifier */
}

/**
 * wake_up_process - Wake up a specific process
 * @p: The process to be woken up.
 *
 * Attempt to wake up the nominated process and move it to the set of runnable
 * processes.
 *
 * Return: 1 if the process was woken up, 0 if it was already running.
 *
 * It may be assumed that this function implies a write memory barrier before
 * changing the task state if and only if any tasks are woken up.
 */
int wake_up_process(struct task_struct *p)
{
	WARN_ON(task_is_stopped_or_traced(p));
	return try_to_wake_up(p, TASK_NORMAL, 0);
}
EXPORT_SYMBOL(wake_up_process);

/**
 * wake_up_process_no_notif - Wake up a specific process without notifying
 * governor
 * @p: The process to be woken up.
 *
 * Attempt to wake up the nominated process and move it to the set of runnable
 * processes.
 *
 * Return: 1 if the process was woken up, 0 if it was already running.
 *
 * It may be assumed that this function implies a write memory barrier before
 * changing the task state if and only if any tasks are woken up.
 */
int wake_up_process_no_notif(struct task_struct *p)
{
	WARN_ON(task_is_stopped_or_traced(p));
	return try_to_wake_up(p, TASK_NORMAL, WF_NO_NOTIFIER);
}
EXPORT_SYMBOL(wake_up_process_no_notif);

int wake_up_state(struct task_struct *p, unsigned int state)
{
	return try_to_wake_up(p, state, 0);
}

/*
 * This function clears the sched_dl_entity static params.
 */
void __dl_clear_params(struct task_struct *p)
{
	struct sched_dl_entity *dl_se = &p->dl;

	dl_se->dl_runtime = 0;
	dl_se->dl_deadline = 0;
	dl_se->dl_period = 0;
	dl_se->flags = 0;
	dl_se->dl_bw = 0;
}

/*
 * Perform scheduler related setup for a newly forked process p.
 * p is forked by current.
 *
 * __sched_fork() is basic setup used by init_idle() too:
 */
static void __sched_fork(unsigned long clone_flags, struct task_struct *p)
{
	p->on_rq			= 0;

	p->se.on_rq			= 0;
	p->se.exec_start		= 0;
	p->se.sum_exec_runtime		= 0;
	p->se.prev_sum_exec_runtime	= 0;
	p->se.nr_migrations		= 0;
	p->se.vruntime			= 0;

	INIT_LIST_HEAD(&p->se.group_node);

#ifdef CONFIG_SCHEDSTATS
	memset(&p->se.statistics, 0, sizeof(p->se.statistics));
#endif

	RB_CLEAR_NODE(&p->dl.rb_node);
	hrtimer_init(&p->dl.dl_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	__dl_clear_params(p);

	INIT_LIST_HEAD(&p->rt.run_list);
	p->rt.timeout		= 0;
	p->rt.time_slice	= sched_rr_timeslice;
	p->rt.on_rq		= 0;
	p->rt.on_list		= 0;

#ifdef CONFIG_PREEMPT_NOTIFIERS
	INIT_HLIST_HEAD(&p->preempt_notifiers);
#endif

#ifdef CONFIG_NUMA_BALANCING
	if (p->mm && atomic_read(&p->mm->mm_users) == 1) {
		p->mm->numa_next_scan = jiffies + msecs_to_jiffies(sysctl_numa_balancing_scan_delay);
		p->mm->numa_scan_seq = 0;
	}

	if (clone_flags & CLONE_VM)
		p->numa_preferred_nid = current->numa_preferred_nid;
	else
		p->numa_preferred_nid = -1;

	p->node_stamp = 0ULL;
	p->numa_scan_seq = p->mm ? p->mm->numa_scan_seq : 0;
	p->numa_scan_period = sysctl_numa_balancing_scan_delay;
	p->numa_work.next = &p->numa_work;
	p->numa_faults_memory = NULL;
	p->numa_faults_buffer_memory = NULL;
	p->last_task_numa_placement = 0;
	p->last_sum_exec_runtime = 0;

	INIT_LIST_HEAD(&p->numa_entry);
	p->numa_group = NULL;
#endif /* CONFIG_NUMA_BALANCING */
}

#ifdef CONFIG_NUMA_BALANCING
#ifdef CONFIG_SCHED_DEBUG
void set_numabalancing_state(bool enabled)
{
	if (enabled)
		sched_feat_set("NUMA");
	else
		sched_feat_set("NO_NUMA");
}
#else
__read_mostly bool numabalancing_enabled;

void set_numabalancing_state(bool enabled)
{
	numabalancing_enabled = enabled;
}
#endif /* CONFIG_SCHED_DEBUG */

#ifdef CONFIG_PROC_SYSCTL
int sysctl_numa_balancing(struct ctl_table *table, int write,
			 void __user *buffer, size_t *lenp, loff_t *ppos)
{
	struct ctl_table t;
	int err;
	int state = numabalancing_enabled;

	if (write && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	t = *table;
	t.data = &state;
	err = proc_dointvec_minmax(&t, write, buffer, lenp, ppos);
	if (err < 0)
		return err;
	if (write)
		set_numabalancing_state(state);
	return err;
}
#endif
#endif

/*
 * fork()/clone()-time setup:
 */
int sched_fork(unsigned long clone_flags, struct task_struct *p)
{
	unsigned long flags;
	int cpu = get_cpu();

	__sched_fork(clone_flags, p);
	/*
	 * We mark the process as running here. This guarantees that
	 * nobody will actually run it, and a signal or other external
	 * event cannot wake it up and insert it on the runqueue either.
	 */
	p->state = TASK_RUNNING;

	/*
	 * Make sure we do not leak PI boosting priority to the child.
	 */
	p->prio = current->normal_prio;

	/*
	 * Revert to default priority/policy on fork if requested.
	 */
	if (unlikely(p->sched_reset_on_fork)) {
		if (task_has_dl_policy(p) || task_has_rt_policy(p)) {
			p->policy = SCHED_NORMAL;
			p->static_prio = NICE_TO_PRIO(0);
			p->rt_priority = 0;
		} else if (PRIO_TO_NICE(p->static_prio) < 0)
			p->static_prio = NICE_TO_PRIO(0);

		p->prio = p->normal_prio = __normal_prio(p);
		set_load_weight(p);

		/*
		 * We don't need the reset flag anymore after the fork. It has
		 * fulfilled its duty:
		 */
		p->sched_reset_on_fork = 0;
	}

	if (dl_prio(p->prio)) {
		put_cpu();
		return -EAGAIN;
	} else if (rt_prio(p->prio)) {
		p->sched_class = &rt_sched_class;
	} else {
		p->sched_class = &fair_sched_class;
	}

	if (p->sched_class->task_fork)
		p->sched_class->task_fork(p);

	/*
	 * The child is not yet in the pid-hash so no cgroup attach races,
	 * and the cgroup is pinned to this child due to cgroup_fork()
	 * is ran before sched_fork().
	 *
	 * Silence PROVE_RCU.
	 */
	raw_spin_lock_irqsave(&p->pi_lock, flags);
	set_task_cpu(p, cpu);
	raw_spin_unlock_irqrestore(&p->pi_lock, flags);

#if defined(CONFIG_SCHEDSTATS) || defined(CONFIG_TASK_DELAY_ACCT)
	if (likely(sched_info_on()))
		memset(&p->sched_info, 0, sizeof(p->sched_info));
#endif
#if defined(CONFIG_SMP)
	p->on_cpu = 0;
#endif
	init_task_preempt_count(p);
#ifdef CONFIG_SMP
	plist_node_init(&p->pushable_tasks, MAX_PRIO);
	RB_CLEAR_NODE(&p->pushable_dl_tasks);
#endif

	put_cpu();
	return 0;
}

unsigned long to_ratio(u64 period, u64 runtime)
{
	if (runtime == RUNTIME_INF)
		return 1ULL << 20;

	/*
	 * Doing this here saves a lot of checks in all
	 * the calling paths, and returning zero seems
	 * safe for them anyway.
	 */
	if (period == 0)
		return 0;

	return div64_u64(runtime << 20, period);
}

#ifdef CONFIG_SMP
inline struct dl_bw *dl_bw_of(int i)
{
	rcu_lockdep_assert(rcu_read_lock_sched_held(),
			   "sched RCU must be held");
	return &cpu_rq(i)->rd->dl_bw;
}

static inline int dl_bw_cpus(int i)
{
	struct root_domain *rd = cpu_rq(i)->rd;
	int cpus = 0;

	rcu_lockdep_assert(rcu_read_lock_sched_held(),
			   "sched RCU must be held");
	for_each_cpu_and(i, rd->span, cpu_active_mask)
		cpus++;

	return cpus;
}
#else
inline struct dl_bw *dl_bw_of(int i)
{
	return &cpu_rq(i)->dl.dl_bw;
}

static inline int dl_bw_cpus(int i)
{
	return 1;
}
#endif

static inline
void __dl_clear(struct dl_bw *dl_b, u64 tsk_bw)
{
	dl_b->total_bw -= tsk_bw;
}

static inline
void __dl_add(struct dl_bw *dl_b, u64 tsk_bw)
{
	dl_b->total_bw += tsk_bw;
}

static inline
bool __dl_overflow(struct dl_bw *dl_b, int cpus, u64 old_bw, u64 new_bw)
{
	return dl_b->bw != -1 &&
	       dl_b->bw * cpus < dl_b->total_bw - old_bw + new_bw;
}

/*
 * We must be sure that accepting a new task (or allowing changing the
 * parameters of an existing one) is consistent with the bandwidth
 * constraints. If yes, this function also accordingly updates the currently
 * allocated bandwidth to reflect the new situation.
 *
 * This function is called while holding p's rq->lock.
 */
static int dl_overflow(struct task_struct *p, int policy,
		       const struct sched_attr *attr)
{

	struct dl_bw *dl_b = dl_bw_of(task_cpu(p));
	u64 period = attr->sched_period ?: attr->sched_deadline;
	u64 runtime = attr->sched_runtime;
	u64 new_bw = dl_policy(policy) ? to_ratio(period, runtime) : 0;
	int cpus, err = -1;

	if (new_bw == p->dl.dl_bw)
		return 0;

	/*
	 * Either if a task, enters, leave, or stays -deadline but changes
	 * its parameters, we may need to update accordingly the total
	 * allocated bandwidth of the container.
	 */
	raw_spin_lock(&dl_b->lock);
	cpus = dl_bw_cpus(task_cpu(p));
	if (dl_policy(policy) && !task_has_dl_policy(p) &&
	    !__dl_overflow(dl_b, cpus, 0, new_bw)) {
		__dl_add(dl_b, new_bw);
		err = 0;
	} else if (dl_policy(policy) && task_has_dl_policy(p) &&
		   !__dl_overflow(dl_b, cpus, p->dl.dl_bw, new_bw)) {
		__dl_clear(dl_b, p->dl.dl_bw);
		__dl_add(dl_b, new_bw);
		err = 0;
	} else if (!dl_policy(policy) && task_has_dl_policy(p)) {
		__dl_clear(dl_b, p->dl.dl_bw);
		err = 0;
	}
	raw_spin_unlock(&dl_b->lock);

	return err;
}

extern void init_dl_bw(struct dl_bw *dl_b);

/*
 * wake_up_new_task - wake up a newly created task for the first time.
 *
 * This function will do some initial scheduler statistics housekeeping
 * that must be done for every newly created context, then puts the task
 * on the runqueue and wakes it.
 */
void wake_up_new_task(struct task_struct *p)
{
	unsigned long flags;
	struct rq *rq;

	raw_spin_lock_irqsave(&p->pi_lock, flags);
	init_new_task_load(p);
	add_new_task_to_grp(p);
#ifdef CONFIG_SMP
	/*
	 * Fork balancing, do it here and not earlier because:
	 *  - cpus_allowed can change in the fork path
	 *  - any previously selected cpu might disappear through hotplug
	 */
	set_task_cpu(p, select_task_rq(p, task_cpu(p), SD_BALANCE_FORK, 0));
#endif

	/* Initialize new task's runnable average */
	init_task_runnable_average(p);
	rq = __task_rq_lock(p);
	mark_task_starting(p);
	activate_task(rq, p, 0);
	p->on_rq = TASK_ON_RQ_QUEUED;
	trace_sched_wakeup_new(p, true);
	check_preempt_curr(rq, p, WF_FORK);
#ifdef CONFIG_SMP
	if (p->sched_class->task_woken)
		p->sched_class->task_woken(rq, p);
#endif
	task_rq_unlock(rq, p, &flags);
}

#ifdef CONFIG_PREEMPT_NOTIFIERS

/**
 * preempt_notifier_register - tell me when current is being preempted & rescheduled
 * @notifier: notifier struct to register
 */
void preempt_notifier_register(struct preempt_notifier *notifier)
{
	hlist_add_head(&notifier->link, &current->preempt_notifiers);
}
EXPORT_SYMBOL_GPL(preempt_notifier_register);

/**
 * preempt_notifier_unregister - no longer interested in preemption notifications
 * @notifier: notifier struct to unregister
 *
 * This is safe to call from within a preemption notifier.
 */
void preempt_notifier_unregister(struct preempt_notifier *notifier)
{
	hlist_del(&notifier->link);
}
EXPORT_SYMBOL_GPL(preempt_notifier_unregister);

static void fire_sched_in_preempt_notifiers(struct task_struct *curr)
{
	struct preempt_notifier *notifier;

	hlist_for_each_entry(notifier, &curr->preempt_notifiers, link)
		notifier->ops->sched_in(notifier, raw_smp_processor_id());
}

static void
fire_sched_out_preempt_notifiers(struct task_struct *curr,
				 struct task_struct *next)
{
	struct preempt_notifier *notifier;

	hlist_for_each_entry(notifier, &curr->preempt_notifiers, link)
		notifier->ops->sched_out(notifier, next);
}

#else /* !CONFIG_PREEMPT_NOTIFIERS */

static void fire_sched_in_preempt_notifiers(struct task_struct *curr)
{
}

static void
fire_sched_out_preempt_notifiers(struct task_struct *curr,
				 struct task_struct *next)
{
}

#endif /* CONFIG_PREEMPT_NOTIFIERS */

/**
 * prepare_task_switch - prepare to switch tasks
 * @rq: the runqueue preparing to switch
 * @prev: the current task that is being switched out
 * @next: the task we are going to switch to.
 *
 * This is called with the rq lock held and interrupts off. It must
 * be paired with a subsequent finish_task_switch after the context
 * switch.
 *
 * prepare_task_switch sets up locking and calls architecture specific
 * hooks.
 */
static inline void
prepare_task_switch(struct rq *rq, struct task_struct *prev,
		    struct task_struct *next)
{
	trace_sched_switch(prev, next);
	sched_info_switch(rq, prev, next);
	perf_event_task_sched_out(prev, next);
	fire_sched_out_preempt_notifiers(prev, next);
	prepare_lock_switch(rq, next);
	prepare_arch_switch(next);

#ifdef CONFIG_MSM_APP_SETTINGS
	if (use_app_setting)
		switch_app_setting_bit(prev, next);

	if (use_32bit_app_setting || use_32bit_app_setting_pro)
		switch_32bit_app_setting_bit(prev, next);
#endif
}

/**
 * finish_task_switch - clean up after a task-switch
 * @rq: runqueue associated with task-switch
 * @prev: the thread we just switched away from.
 *
 * finish_task_switch must be called after the context switch, paired
 * with a prepare_task_switch call before the context switch.
 * finish_task_switch will reconcile locking set up by prepare_task_switch,
 * and do any other architecture-specific cleanup actions.
 *
 * Note that we may have delayed dropping an mm in context_switch(). If
 * so, we finish that here outside of the runqueue lock. (Doing it
 * with the lock held can cause deadlocks; see schedule() for
 * details.)
 */
static void finish_task_switch(struct rq *rq, struct task_struct *prev)
	__releases(rq->lock)
{
	struct mm_struct *mm = rq->prev_mm;
	long prev_state;

	rq->prev_mm = NULL;

	/*
	 * A task struct has one reference for the use as "current".
	 * If a task dies, then it sets TASK_DEAD in tsk->state and calls
	 * schedule one last time. The schedule call will never return, and
	 * the scheduled task must drop that reference.
	 *
	 * We must observe prev->state before clearing prev->on_cpu (in
	 * finish_lock_switch), otherwise a concurrent wakeup can get prev
	 * running on another CPU and we could rave with its RUNNING -> DEAD
	 * transition, resulting in a double drop.
	 */
	prev_state = prev->state;
	vtime_task_switch(prev);
	finish_arch_switch(prev);
	perf_event_task_sched_in(prev, current);
	finish_lock_switch(rq, prev);
	finish_arch_post_lock_switch();

	fire_sched_in_preempt_notifiers(current);
	if (mm)
		mmdrop(mm);
	if (unlikely(prev_state == TASK_DEAD)) {
		if (prev->sched_class->task_dead)
			prev->sched_class->task_dead(prev);

		/*
		 * Remove function-return probe instances associated with this
		 * task and put them back on the free list.
		 */
		kprobe_flush_task(prev);
		put_task_struct(prev);
	}

	tick_nohz_task_switch(current);
}

#ifdef CONFIG_SMP

/* rq->lock is NOT held, but preemption is disabled */
static inline void post_schedule(struct rq *rq)
{
	if (rq->post_schedule) {
		unsigned long flags;

		raw_spin_lock_irqsave(&rq->lock, flags);
		if (rq->curr->sched_class->post_schedule)
			rq->curr->sched_class->post_schedule(rq);
		raw_spin_unlock_irqrestore(&rq->lock, flags);

		rq->post_schedule = 0;
	}
}

#else

static inline void post_schedule(struct rq *rq)
{
}

#endif

/**
 * schedule_tail - first thing a freshly forked thread must call.
 * @prev: the thread we just switched away from.
 */
asmlinkage __visible void schedule_tail(struct task_struct *prev)
	__releases(rq->lock)
{
	struct rq *rq = this_rq();

	finish_task_switch(rq, prev);

	/*
	 * FIXME: do we need to worry about rq being invalidated by the
	 * task_switch?
	 */
	post_schedule(rq);

	if (current->set_child_tid)
		put_user(task_pid_vnr(current), current->set_child_tid);
}

/*
 * context_switch - switch to the new MM and the new
 * thread's register state.
 */
static inline void
context_switch(struct rq *rq, struct task_struct *prev,
	       struct task_struct *next)
{
	struct mm_struct *mm, *oldmm;

	prepare_task_switch(rq, prev, next);

	mm = next->mm;
	oldmm = prev->active_mm;
	/*
	 * For paravirt, this is coupled with an exit in switch_to to
	 * combine the page table reload and the switch backend into
	 * one hypercall.
	 */
	arch_start_context_switch(prev);

	if (!mm) {
		next->active_mm = oldmm;
		atomic_inc(&oldmm->mm_count);
		enter_lazy_tlb(oldmm, next);
	} else
		switch_mm(oldmm, mm, next);

	if (!prev->mm) {
		prev->active_mm = NULL;
		rq->prev_mm = oldmm;
	}
	/*
	 * Since the runqueue lock will be released by the next
	 * task (which is an invalid locking op but in the case
	 * of the scheduler it's an obvious special-case), so we
	 * do an early lockdep release here:
	 */
	spin_release(&rq->lock.dep_map, 1, _THIS_IP_);

	context_tracking_task_switch(prev, next);
	/* Here we just switch the register state and the stack. */
	switch_to(prev, next, prev);

	barrier();
	/*
	 * this_rq must be evaluated again because prev may have moved
	 * CPUs since it called schedule(), thus the 'rq' on its stack
	 * frame will be invalid.
	 */
	finish_task_switch(this_rq(), prev);
}

/*
 * nr_running and nr_context_switches:
 *
 * externally visible scheduler statistics: current number of runnable
 * threads, total number of context switches performed since bootup.
 */
unsigned long nr_running(void)
{
	unsigned long i, sum = 0;

	for_each_online_cpu(i)
		sum += cpu_rq(i)->nr_running;

	return sum;
}

/*
 * Check if only the current task is running on the cpu.
 *
 * Caution: this function does not check that the caller has disabled
 * preemption, thus the result might have a time-of-check-to-time-of-use
 * race.  The caller is responsible to use it correctly, for example:
 *
 * - from a non-preemptable section (of course)
 *
 * - from a thread that is bound to a single CPU
 *
 * - in a loop with very short iterations (e.g. a polling loop)
 */
bool single_task_running(void)
{
	return raw_rq()->nr_running == 1;
}
EXPORT_SYMBOL(single_task_running);

unsigned long long nr_context_switches(void)
{
	int i;
	unsigned long long sum = 0;

	for_each_possible_cpu(i)
		sum += cpu_rq(i)->nr_switches;

	return sum;
}

unsigned long nr_iowait(void)
{
	unsigned long i, sum = 0;

	for_each_possible_cpu(i)
		sum += atomic_read(&cpu_rq(i)->nr_iowait);

	return sum;
}

unsigned long nr_iowait_cpu(int cpu)
{
	struct rq *this = cpu_rq(cpu);
	return atomic_read(&this->nr_iowait);
}

void get_iowait_load(unsigned long *nr_waiters, unsigned long *load)
{
	struct rq *this = this_rq();
	*nr_waiters = atomic_read(&this->nr_iowait);
	*load = this->cpu_load[0];
}

#if defined(CONFIG_SMP)

/*
 * sched_exec - execve() is a valuable balancing opportunity, because at
 * this point the task has the smallest effective memory and cache footprint.
 */
void sched_exec(void)
{
	struct task_struct *p = current;
	unsigned long flags;
	int dest_cpu, curr_cpu;

	if (sched_enable_hmp)
		return;

	raw_spin_lock_irqsave(&p->pi_lock, flags);
	curr_cpu = task_cpu(p);
	dest_cpu = p->sched_class->select_task_rq(p, task_cpu(p), SD_BALANCE_EXEC, 0);
	if (dest_cpu == smp_processor_id())
		goto unlock;

	if (likely(cpu_active(dest_cpu))) {
		struct migration_arg arg = { p, dest_cpu };

		raw_spin_unlock_irqrestore(&p->pi_lock, flags);
		stop_one_cpu(curr_cpu, migration_cpu_stop, &arg);
		return;
	}
unlock:
	raw_spin_unlock_irqrestore(&p->pi_lock, flags);
}

#endif

DEFINE_PER_CPU(struct kernel_stat, kstat);
DEFINE_PER_CPU(struct kernel_cpustat, kernel_cpustat);

EXPORT_PER_CPU_SYMBOL(kstat);
EXPORT_PER_CPU_SYMBOL(kernel_cpustat);

/*
 * Return accounted runtime for the task.
 * In case the task is currently running, return the runtime plus current's
 * pending runtime that have not been accounted yet.
 */
unsigned long long task_sched_runtime(struct task_struct *p)
{
	unsigned long flags;
	struct rq *rq;
	u64 ns;

#if defined(CONFIG_64BIT) && defined(CONFIG_SMP)
	/*
	 * 64-bit doesn't need locks to atomically read a 64bit value.
	 * So we have a optimization chance when the task's delta_exec is 0.
	 * Reading ->on_cpu is racy, but this is ok.
	 *
	 * If we race with it leaving cpu, we'll take a lock. So we're correct.
	 * If we race with it entering cpu, unaccounted time is 0. This is
	 * indistinguishable from the read occurring a few cycles earlier.
	 * If we see ->on_cpu without ->on_rq, the task is leaving, and has
	 * been accounted, so we're correct here as well.
	 */
	if (!p->on_cpu || !task_on_rq_queued(p))
		return p->se.sum_exec_runtime;
#endif

	rq = task_rq_lock(p, &flags);
	/*
	 * Must be ->curr _and_ ->on_rq.  If dequeued, we would
	 * project cycles that may never be accounted to this
	 * thread, breaking clock_gettime().
	 */
	if (task_current(rq, p) && task_on_rq_queued(p)) {
		update_rq_clock(rq);
		p->sched_class->update_curr(rq);
	}
	ns = p->se.sum_exec_runtime;
	task_rq_unlock(rq, p, &flags);

	return ns;
}

#ifdef CONFIG_SCHED_HMP
static bool early_detection_notify(struct rq *rq, u64 wallclock)
{
	struct task_struct *p;
	int loop_max = 10;

	if (!sched_boost() || !rq->cfs.h_nr_running)
		return 0;

	rq->ed_task = NULL;
	list_for_each_entry(p, &rq->cfs_tasks, se.group_node) {
		if (!loop_max)
			break;

		if (wallclock - p->last_wake_ts >= EARLY_DETECTION_DURATION) {
			rq->ed_task = p;
			return 1;
		}

		loop_max--;
	}

	return 0;
}
#else /* CONFIG_SCHED_HMP */
static bool early_detection_notify(struct rq *rq, u64 wallclock)
{
	return 0;
}
#endif /* CONFIG_SCHED_HMP */

/*
 * This function gets called by the timer code, with HZ frequency.
 * We call it with interrupts disabled.
 */
void scheduler_tick(void)
{
	int cpu = smp_processor_id();
	struct rq *rq = cpu_rq(cpu);
	struct task_struct *curr = rq->curr;
	u64 wallclock;
	bool early_notif;
	u32 old_load;
	struct related_thread_group *grp;

	sched_clock_tick();

	raw_spin_lock(&rq->lock);
	old_load = task_load(curr);
	set_window_start(rq);
	update_rq_clock(rq);
	curr->sched_class->task_tick(rq, curr, 0);
	update_cpu_load_active(rq);
	wallclock = sched_ktime_clock();
	update_task_ravg(rq->curr, rq, TASK_UPDATE, wallclock, 0);
	early_notif = early_detection_notify(rq, wallclock);
	raw_spin_unlock(&rq->lock);

	if (early_notif)
		atomic_notifier_call_chain(&load_alert_notifier_head,
					0, (void *)(long)cpu);

	perf_event_task_tick();

#ifdef CONFIG_SMP
	rq->idle_balance = idle_cpu(cpu);
	trigger_load_balance(rq);
#endif
	rq_last_tick_reset(rq);

	rcu_read_lock();
	grp = task_related_thread_group(curr);
	if (update_preferred_cluster(grp, curr, old_load))
		set_preferred_cluster(grp);
	rcu_read_unlock();

	if (curr->sched_class == &fair_sched_class)
		check_for_migration(rq, curr);
}

#ifdef CONFIG_NO_HZ_FULL
/**
 * scheduler_tick_max_deferment
 *
 * Keep at least one tick per second when a single
 * active task is running because the scheduler doesn't
 * yet completely support full dynticks environment.
 *
 * This makes sure that uptime, CFS vruntime, load
 * balancing, etc... continue to move forward, even
 * with a very low granularity.
 *
 * Return: Maximum deferment in nanoseconds.
 */
u64 scheduler_tick_max_deferment(void)
{
	struct rq *rq = this_rq();
	unsigned long next, now = ACCESS_ONCE(jiffies);

	next = rq->last_sched_tick + HZ;

	if (time_before_eq(next, now))
		return 0;

	return jiffies_to_nsecs(next - now);
}
#endif

notrace unsigned long get_parent_ip(unsigned long addr)
{
	if (in_lock_functions(addr)) {
		addr = CALLER_ADDR2;
		if (in_lock_functions(addr))
			addr = CALLER_ADDR3;
	}
	return addr;
}

#if defined(CONFIG_PREEMPT) && (defined(CONFIG_DEBUG_PREEMPT) || \
				defined(CONFIG_PREEMPT_TRACER))

void preempt_count_add(int val)
{
#ifdef CONFIG_DEBUG_PREEMPT
	/*
	 * Underflow?
	 */
	if (DEBUG_LOCKS_WARN_ON((preempt_count() < 0)))
		return;
#endif
	__preempt_count_add(val);
#ifdef CONFIG_DEBUG_PREEMPT
	/*
	 * Spinlock count overflowing soon?
	 */
	DEBUG_LOCKS_WARN_ON((preempt_count() & PREEMPT_MASK) >=
				PREEMPT_MASK - 10);
#endif
	if (preempt_count() == val) {
		unsigned long ip = get_parent_ip(CALLER_ADDR1);
#ifdef CONFIG_DEBUG_PREEMPT
		current->preempt_disable_ip = ip;
#endif
		trace_preempt_off(CALLER_ADDR0, ip);
	}
}
EXPORT_SYMBOL(preempt_count_add);
NOKPROBE_SYMBOL(preempt_count_add);

void preempt_count_sub(int val)
{
#ifdef CONFIG_DEBUG_PREEMPT
	/*
	 * Underflow?
	 */
	if (DEBUG_LOCKS_WARN_ON(val > preempt_count()))
		return;
	/*
	 * Is the spinlock portion underflowing?
	 */
	if (DEBUG_LOCKS_WARN_ON((val < PREEMPT_MASK) &&
			!(preempt_count() & PREEMPT_MASK)))
		return;
#endif

	if (preempt_count() == val)
		trace_preempt_on(CALLER_ADDR0, get_parent_ip(CALLER_ADDR1));
	__preempt_count_sub(val);
}
EXPORT_SYMBOL(preempt_count_sub);
NOKPROBE_SYMBOL(preempt_count_sub);

#endif

/*
 * Print scheduling while atomic bug:
 */
static noinline void __schedule_bug(struct task_struct *prev)
{
	if (oops_in_progress)
		return;

	printk(KERN_ERR "BUG: scheduling while atomic: %s/%d/0x%08x\n",
		prev->comm, prev->pid, preempt_count());

	debug_show_held_locks(prev);
	print_modules();
	if (irqs_disabled())
		print_irqtrace_events(prev);
#ifdef CONFIG_DEBUG_PREEMPT
	if (in_atomic_preempt_off()) {
		pr_err("Preemption disabled at:");
		print_ip_sym(current->preempt_disable_ip);
		pr_cont("\n");
	}
#endif
#ifdef CONFIG_PANIC_ON_SCHED_BUG
	BUG();
#endif
	dump_stack();
	add_taint(TAINT_WARN, LOCKDEP_STILL_OK);
}

/*
 * Various schedule()-time debugging checks and statistics:
 */
static inline void schedule_debug(struct task_struct *prev)
{
#ifdef CONFIG_SCHED_STACK_END_CHECK
	if (unlikely(task_stack_end_corrupted(prev)))
		panic("corrupted stack end detected inside scheduler\n");
#endif
	/*
	 * Test if we are atomic. Since do_exit() needs to call into
	 * schedule() atomically, we ignore that path. Otherwise whine
	 * if we are scheduling when we should not.
	 */
	if (unlikely(in_atomic_preempt_off() && prev->state != TASK_DEAD))
		__schedule_bug(prev);
	rcu_sleep_check();

	profile_hit(SCHED_PROFILING, __builtin_return_address(0));

	schedstat_inc(this_rq(), sched_count);
}

/*
 * Pick up the highest-prio task:
 */
static inline struct task_struct *
pick_next_task(struct rq *rq, struct task_struct *prev)
{
	const struct sched_class *class = &fair_sched_class;
	struct task_struct *p;

	/*
	 * Optimization: we know that if all tasks are in
	 * the fair class we can call that function directly:
	 */
	if (likely(prev->sched_class == class &&
		   rq->nr_running == rq->cfs.h_nr_running)) {
		p = fair_sched_class.pick_next_task(rq, prev);
		if (unlikely(p == RETRY_TASK))
			goto again;

		/* assumes fair_sched_class->next == idle_sched_class */
		if (unlikely(!p))
			p = idle_sched_class.pick_next_task(rq, prev);

		return p;
	}

again:
	for_each_class(class) {
		p = class->pick_next_task(rq, prev);
		if (p) {
			if (unlikely(p == RETRY_TASK))
				goto again;
			return p;
		}
	}

	BUG(); /* the idle class will always have a runnable task */
}

/*
 * __schedule() is the main scheduler function.
 *
 * The main means of driving the scheduler and thus entering this function are:
 *
 *   1. Explicit blocking: mutex, semaphore, waitqueue, etc.
 *
 *   2. TIF_NEED_RESCHED flag is checked on interrupt and userspace return
 *      paths. For example, see arch/x86/entry_64.S.
 *
 *      To drive preemption between tasks, the scheduler sets the flag in timer
 *      interrupt handler scheduler_tick().
 *
 *   3. Wakeups don't really cause entry into schedule(). They add a
 *      task to the run-queue and that's it.
 *
 *      Now, if the new task added to the run-queue preempts the current
 *      task, then the wakeup sets TIF_NEED_RESCHED and schedule() gets
 *      called on the nearest possible occasion:
 *
 *       - If the kernel is preemptible (CONFIG_PREEMPT=y):
 *
 *         - in syscall or exception context, at the next outmost
 *           preempt_enable(). (this might be as soon as the wake_up()'s
 *           spin_unlock()!)
 *
 *         - in IRQ context, return from interrupt-handler to
 *           preemptible context
 *
 *       - If the kernel is not preemptible (CONFIG_PREEMPT is not set)
 *         then at the next:
 *
 *          - cond_resched() call
 *          - explicit schedule() call
 *          - return from syscall or exception to user-space
 *          - return from interrupt-handler to user-space
 */
static void __sched __schedule(void)
{
	struct task_struct *prev, *next;
	unsigned long *switch_count;
	struct rq *rq;
	int cpu;
	u64 wallclock;

need_resched:
	preempt_disable();
	cpu = smp_processor_id();
	rq = cpu_rq(cpu);
	rcu_note_context_switch(cpu);
	prev = rq->curr;

	schedule_debug(prev);

	if (sched_feat(HRTICK))
		hrtick_clear(rq);

	/*
	 * Make sure that signal_pending_state()->signal_pending() below
	 * can't be reordered with __set_current_state(TASK_INTERRUPTIBLE)
	 * done by the caller to avoid the race with signal_wake_up().
	 */
	smp_mb__before_spinlock();
	raw_spin_lock_irq(&rq->lock);

	switch_count = &prev->nivcsw;
	if (prev->state && !(preempt_count() & PREEMPT_ACTIVE)) {
		if (unlikely(signal_pending_state(prev->state, prev))) {
			prev->state = TASK_RUNNING;
		} else {
			deactivate_task(rq, prev, DEQUEUE_SLEEP);
			prev->on_rq = 0;

			/*
			 * If a worker went to sleep, notify and ask workqueue
			 * whether it wants to wake up a task to maintain
			 * concurrency.
			 */
			if (prev->flags & PF_WQ_WORKER) {
				struct task_struct *to_wakeup;

				to_wakeup = wq_worker_sleeping(prev, cpu);
				if (to_wakeup)
					try_to_wake_up_local(to_wakeup);
			}
		}
		switch_count = &prev->nvcsw;
	}

	if (task_on_rq_queued(prev) || rq->skip_clock_update < 0)
		update_rq_clock(rq);

	next = pick_next_task(rq, prev);
	wallclock = sched_ktime_clock();
	update_task_ravg(prev, rq, PUT_PREV_TASK, wallclock, 0);
	update_task_ravg(next, rq, PICK_NEXT_TASK, wallclock, 0);
	clear_tsk_need_resched(prev);
	clear_preempt_need_resched();
	rq->skip_clock_update = 0;

	BUG_ON(task_cpu(next) != cpu_of(rq));

	if (likely(prev != next)) {
		rq->nr_switches++;
		rq->curr = next;
		++*switch_count;

		set_task_last_switch_out(prev, wallclock);

		context_switch(rq, prev, next); /* unlocks the rq */
		/*
		 * The context switch have flipped the stack from under us
		 * and restored the local variables which were saved when
		 * this task called schedule() in the past. prev == current
		 * is still correct, but it can be moved to another cpu/rq.
		 */
		cpu = smp_processor_id();
		rq = cpu_rq(cpu);
	} else
		raw_spin_unlock_irq(&rq->lock);

	post_schedule(rq);

	sched_preempt_enable_no_resched();
	if (need_resched())
		goto need_resched;
}

static inline void sched_submit_work(struct task_struct *tsk)
{
	if (!tsk->state || tsk_is_pi_blocked(tsk))
		return;
	/*
	 * If we are going to sleep and we have plugged IO queued,
	 * make sure to submit it to avoid deadlocks.
	 */
	if (blk_needs_flush_plug(tsk))
		blk_schedule_flush_plug(tsk);
}

asmlinkage __visible void __sched schedule(void)
{
	struct task_struct *tsk = current;

	sched_submit_work(tsk);
	__schedule();
}
EXPORT_SYMBOL(schedule);

#ifdef CONFIG_CONTEXT_TRACKING
asmlinkage __visible void __sched schedule_user(void)
{
	/*
	 * If we come here after a random call to set_need_resched(),
	 * or we have been woken up remotely but the IPI has not yet arrived,
	 * we haven't yet exited the RCU idle mode. Do it here manually until
	 * we find a better solution.
	 *
	 * NB: There are buggy callers of this function.  Ideally we
	 * should warn if prev_state != IN_USER, but that will trigger
	 * too frequently to make sense yet.
	 */
	enum ctx_state prev_state = exception_enter();
	schedule();
	exception_exit(prev_state);
}
#endif

/**
 * schedule_preempt_disabled - called with preemption disabled
 *
 * Returns with preemption disabled. Note: preempt_count must be 1
 */
void __sched schedule_preempt_disabled(void)
{
	sched_preempt_enable_no_resched();
	schedule();
	preempt_disable();
}

#ifdef CONFIG_PREEMPT
/*
 * this is the entry point to schedule() from in-kernel preemption
 * off of preempt_enable. Kernel preemptions off return from interrupt
 * occur there and call schedule directly.
 */
asmlinkage __visible void __sched notrace preempt_schedule(void)
{
	/*
	 * If there is a non-zero preempt_count or interrupts are disabled,
	 * we do not want to preempt the current task. Just return..
	 */
	if (likely(!preemptible()))
		return;

	do {
		__preempt_count_add(PREEMPT_ACTIVE);
		__schedule();
		__preempt_count_sub(PREEMPT_ACTIVE);

		/*
		 * Check again in case we missed a preemption opportunity
		 * between schedule and now.
		 */
		barrier();
	} while (need_resched());
}
NOKPROBE_SYMBOL(preempt_schedule);
EXPORT_SYMBOL(preempt_schedule);

#ifdef CONFIG_CONTEXT_TRACKING
/**
 * preempt_schedule_context - preempt_schedule called by tracing
 *
 * The tracing infrastructure uses preempt_enable_notrace to prevent
 * recursion and tracing preempt enabling caused by the tracing
 * infrastructure itself. But as tracing can happen in areas coming
 * from userspace or just about to enter userspace, a preempt enable
 * can occur before user_exit() is called. This will cause the scheduler
 * to be called when the system is still in usermode.
 *
 * To prevent this, the preempt_enable_notrace will use this function
 * instead of preempt_schedule() to exit user context if needed before
 * calling the scheduler.
 */
asmlinkage __visible void __sched notrace preempt_schedule_context(void)
{
	enum ctx_state prev_ctx;

	if (likely(!preemptible()))
		return;

	do {
		__preempt_count_add(PREEMPT_ACTIVE);
		/*
		 * Needs preempt disabled in case user_exit() is traced
		 * and the tracer calls preempt_enable_notrace() causing
		 * an infinite recursion.
		 */
		prev_ctx = exception_enter();
		__schedule();
		exception_exit(prev_ctx);

		__preempt_count_sub(PREEMPT_ACTIVE);
		barrier();
	} while (need_resched());
}
EXPORT_SYMBOL_GPL(preempt_schedule_context);
#endif /* CONFIG_CONTEXT_TRACKING */

#endif /* CONFIG_PREEMPT */

/*
 * this is the entry point to schedule() from kernel preemption
 * off of irq context.
 * Note, that this is called and return with irqs disabled. This will
 * protect us against recursive calling from irq.
 */
asmlinkage __visible void __sched preempt_schedule_irq(void)
{
	enum ctx_state prev_state;

	/* Catch callers which need to be fixed */
	BUG_ON(preempt_count() || !irqs_disabled());

	prev_state = exception_enter();

	do {
		__preempt_count_add(PREEMPT_ACTIVE);
		local_irq_enable();
		__schedule();
		local_irq_disable();
		__preempt_count_sub(PREEMPT_ACTIVE);

		/*
		 * Check again in case we missed a preemption opportunity
		 * between schedule and now.
		 */
		barrier();
	} while (need_resched());

	exception_exit(prev_state);
}

int default_wake_function(wait_queue_t *curr, unsigned mode, int wake_flags,
			  void *key)
{
	return try_to_wake_up(curr->private, mode, wake_flags);
}
EXPORT_SYMBOL(default_wake_function);

#ifdef CONFIG_RT_MUTEXES

/*
 * rt_mutex_setprio - set the current priority of a task
 * @p: task
 * @prio: prio value (kernel-internal form)
 *
 * This function changes the 'effective' priority of a task. It does
 * not touch ->normal_prio like __setscheduler().
 *
 * Used by the rt_mutex code to implement priority inheritance
 * logic. Call site only calls if the priority of the task changed.
 */
void rt_mutex_setprio(struct task_struct *p, int prio)
{
	int oldprio, queued, running, queue_flag = DEQUEUE_SAVE | DEQUEUE_MOVE;
	struct rq *rq;
	const struct sched_class *prev_class;

	BUG_ON(prio > MAX_PRIO);

	rq = __task_rq_lock(p);

	/*
	 * Idle task boosting is a nono in general. There is one
	 * exception, when PREEMPT_RT and NOHZ is active:
	 *
	 * The idle task calls get_next_timer_interrupt() and holds
	 * the timer wheel base->lock on the CPU and another CPU wants
	 * to access the timer (probably to cancel it). We can safely
	 * ignore the boosting request, as the idle CPU runs this code
	 * with interrupts disabled and will complete the lock
	 * protected section without being interrupted. So there is no
	 * real need to boost.
	 */
	if (unlikely(p == rq->idle)) {
		WARN_ON(p != rq->curr);
		WARN_ON(p->pi_blocked_on);
		goto out_unlock;
	}

	trace_sched_pi_setprio(p, prio);
	oldprio = p->prio;

	if (oldprio == prio)
		queue_flag &= ~DEQUEUE_MOVE;

	prev_class = p->sched_class;
	queued = task_on_rq_queued(p);
	running = task_current(rq, p);
	if (queued)
		dequeue_task(rq, p, queue_flag);
	if (running)
		put_prev_task(rq, p);

	/*
	 * Boosting condition are:
	 * 1. -rt task is running and holds mutex A
	 *      --> -dl task blocks on mutex A
	 *
	 * 2. -dl task is running and holds mutex A
	 *      --> -dl task blocks on mutex A and could preempt the
	 *          running task
	 */
	if (dl_prio(prio)) {
		struct task_struct *pi_task = rt_mutex_get_top_task(p);
		if (!dl_prio(p->normal_prio) ||
		    (pi_task && dl_entity_preempt(&pi_task->dl, &p->dl))) {
			p->dl.dl_boosted = 1;
			p->dl.dl_throttled = 0;
			queue_flag |= ENQUEUE_REPLENISH;
		} else
			p->dl.dl_boosted = 0;
		p->sched_class = &dl_sched_class;
	} else if (rt_prio(prio)) {
		if (dl_prio(oldprio))
			p->dl.dl_boosted = 0;
		if (oldprio < prio)
			queue_flag |= ENQUEUE_HEAD;
		p->sched_class = &rt_sched_class;
	} else {
		if (dl_prio(oldprio))
			p->dl.dl_boosted = 0;
		if (rt_prio(oldprio))
			p->rt.timeout = 0;
		p->sched_class = &fair_sched_class;
	}

	p->prio = prio;

	if (running)
		p->sched_class->set_curr_task(rq);
	if (queued)
		enqueue_task(rq, p, queue_flag);

	check_class_changed(rq, p, prev_class, oldprio);
out_unlock:
	__task_rq_unlock(rq);
}
#endif

void set_user_nice(struct task_struct *p, long nice)
{
	int old_prio, delta, queued;
	unsigned long flags;
	struct rq *rq;

	if (task_nice(p) == nice || nice < MIN_NICE || nice > MAX_NICE)
		return;
	/*
	 * We have to be careful, if called from sys_setpriority(),
	 * the task might be in the middle of scheduling on another CPU.
	 */
	rq = task_rq_lock(p, &flags);
	/*
	 * The RT priorities are set via sched_setscheduler(), but we still
	 * allow the 'normal' nice value to be set - but as expected
	 * it wont have any effect on scheduling until the task is
	 * SCHED_DEADLINE, SCHED_FIFO or SCHED_RR:
	 */
	if (task_has_dl_policy(p) || task_has_rt_policy(p)) {
		p->static_prio = NICE_TO_PRIO(nice);
		goto out_unlock;
	}
	queued = task_on_rq_queued(p);
	if (queued)
		dequeue_task(rq, p, DEQUEUE_SAVE);

	p->static_prio = NICE_TO_PRIO(nice);
	set_load_weight(p);
	old_prio = p->prio;
	p->prio = effective_prio(p);
	delta = p->prio - old_prio;

	if (queued) {
		enqueue_task(rq, p, ENQUEUE_RESTORE);
		/*
		 * If the task increased its priority or is running and
		 * lowered its priority, then reschedule its CPU:
		 */
		if (delta < 0 || (delta > 0 && task_running(rq, p)))
			resched_curr(rq);
	}
out_unlock:
	task_rq_unlock(rq, p, &flags);
}
EXPORT_SYMBOL(set_user_nice);

/*
 * can_nice - check if a task can reduce its nice value
 * @p: task
 * @nice: nice value
 */
int can_nice(const struct task_struct *p, const int nice)
{
	/* convert nice value [19,-20] to rlimit style value [1,40] */
	int nice_rlim = nice_to_rlimit(nice);

	return (nice_rlim <= task_rlimit(p, RLIMIT_NICE) ||
		capable(CAP_SYS_NICE));
}

#ifdef __ARCH_WANT_SYS_NICE

/*
 * sys_nice - change the priority of the current process.
 * @increment: priority increment
 *
 * sys_setpriority is a more generic, but much slower function that
 * does similar things.
 */
SYSCALL_DEFINE1(nice, int, increment)
{
	long nice, retval;

	/*
	 * Setpriority might change our priority at the same moment.
	 * We don't have to worry. Conceptually one call occurs first
	 * and we have a single winner.
	 */
	increment = clamp(increment, -NICE_WIDTH, NICE_WIDTH);
	nice = task_nice(current) + increment;

	nice = clamp_val(nice, MIN_NICE, MAX_NICE);
	if (increment < 0 && !can_nice(current, nice))
		return -EPERM;

	retval = security_task_setnice(current, nice);
	if (retval)
		return retval;

	set_user_nice(current, nice);
	return 0;
}

#endif

/**
 * task_prio - return the priority value of a given task.
 * @p: the task in question.
 *
 * Return: The priority value as seen by users in /proc.
 * RT tasks are offset by -200. Normal tasks are centered
 * around 0, value goes from -16 to +15.
 */
int task_prio(const struct task_struct *p)
{
	return p->prio - MAX_RT_PRIO;
}

/**
 * idle_cpu - is a given cpu idle currently?
 * @cpu: the processor in question.
 *
 * Return: 1 if the CPU is currently idle. 0 otherwise.
 */
int idle_cpu(int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	if (rq->curr != rq->idle)
		return 0;

	if (rq->nr_running)
		return 0;

#ifdef CONFIG_SMP
	if (!llist_empty(&rq->wake_list))
		return 0;
#endif

	return 1;
}

/**
 * idle_task - return the idle task for a given cpu.
 * @cpu: the processor in question.
 *
 * Return: The idle task for the cpu @cpu.
 */
struct task_struct *idle_task(int cpu)
{
	return cpu_rq(cpu)->idle;
}

/**
 * find_process_by_pid - find a process with a matching PID value.
 * @pid: the pid in question.
 *
 * The task of @pid, if found. %NULL otherwise.
 */
static struct task_struct *find_process_by_pid(pid_t pid)
{
	return pid ? find_task_by_vpid(pid) : current;
}

/*
 * This function initializes the sched_dl_entity of a newly becoming
 * SCHED_DEADLINE task.
 *
 * Only the static values are considered here, the actual runtime and the
 * absolute deadline will be properly calculated when the task is enqueued
 * for the first time with its new policy.
 */
static void
__setparam_dl(struct task_struct *p, const struct sched_attr *attr)
{
	struct sched_dl_entity *dl_se = &p->dl;

	init_dl_task_timer(dl_se);
	dl_se->dl_runtime = attr->sched_runtime;
	dl_se->dl_deadline = attr->sched_deadline;
	dl_se->dl_period = attr->sched_period ?: dl_se->dl_deadline;
	dl_se->flags = attr->sched_flags;
	dl_se->dl_bw = to_ratio(dl_se->dl_period, dl_se->dl_runtime);
	dl_se->dl_throttled = 0;
	dl_se->dl_new = 1;
	dl_se->dl_yielded = 0;
}

/*
 * sched_setparam() passes in -1 for its policy, to let the functions
 * it calls know not to change it.
 */
#define SETPARAM_POLICY	-1

static void __setscheduler_params(struct task_struct *p,
		const struct sched_attr *attr)
{
	int policy = attr->sched_policy;

	if (policy == SETPARAM_POLICY)
		policy = p->policy;

	p->policy = policy;

	if (dl_policy(policy))
		__setparam_dl(p, attr);
	else if (fair_policy(policy))
		p->static_prio = NICE_TO_PRIO(attr->sched_nice);

	/*
	 * __sched_setscheduler() ensures attr->sched_priority == 0 when
	 * !rt_policy. Always setting this ensures that things like
	 * getparam()/getattr() don't report silly values for !rt tasks.
	 */
	p->rt_priority = attr->sched_priority;
	p->normal_prio = normal_prio(p);
	set_load_weight(p);
}

/* Actually do priority change: must hold pi & rq lock. */
static void __setscheduler(struct rq *rq, struct task_struct *p,
			   const struct sched_attr *attr, bool keep_boost)
{
	__setscheduler_params(p, attr);

	/*
	 * Keep a potential priority boosting if called from
	 * sched_setscheduler().
	 */
	if (keep_boost)
		p->prio = rt_mutex_get_effective_prio(p, normal_prio(p));
	else
		p->prio = normal_prio(p);

	if (dl_prio(p->prio))
		p->sched_class = &dl_sched_class;
	else if (rt_prio(p->prio))
		p->sched_class = &rt_sched_class;
	else
		p->sched_class = &fair_sched_class;
}

static void
__getparam_dl(struct task_struct *p, struct sched_attr *attr)
{
	struct sched_dl_entity *dl_se = &p->dl;

	attr->sched_priority = p->rt_priority;
	attr->sched_runtime = dl_se->dl_runtime;
	attr->sched_deadline = dl_se->dl_deadline;
	attr->sched_period = dl_se->dl_period;
	attr->sched_flags = dl_se->flags;
}

/*
 * This function validates the new parameters of a -deadline task.
 * We ask for the deadline not being zero, and greater or equal
 * than the runtime, as well as the period of being zero or
 * greater than deadline. Furthermore, we have to be sure that
 * user parameters are above the internal resolution of 1us (we
 * check sched_runtime only since it is always the smaller one) and
 * below 2^63 ns (we have to check both sched_deadline and
 * sched_period, as the latter can be zero).
 */
static bool
__checkparam_dl(const struct sched_attr *attr)
{
	/* deadline != 0 */
	if (attr->sched_deadline == 0)
		return false;

	/*
	 * Since we truncate DL_SCALE bits, make sure we're at least
	 * that big.
	 */
	if (attr->sched_runtime < (1ULL << DL_SCALE))
		return false;

	/*
	 * Since we use the MSB for wrap-around and sign issues, make
	 * sure it's not set (mind that period can be equal to zero).
	 */
	if (attr->sched_deadline & (1ULL << 63) ||
	    attr->sched_period & (1ULL << 63))
		return false;

	/* runtime <= deadline <= period (if period != 0) */
	if ((attr->sched_period != 0 &&
	     attr->sched_period < attr->sched_deadline) ||
	    attr->sched_deadline < attr->sched_runtime)
		return false;

	return true;
}

/*
 * check the target process has a UID that matches the current process's
 */
static bool check_same_owner(struct task_struct *p)
{
	const struct cred *cred = current_cred(), *pcred;
	bool match;

	rcu_read_lock();
	pcred = __task_cred(p);
	match = (uid_eq(cred->euid, pcred->euid) ||
		 uid_eq(cred->euid, pcred->uid));
	rcu_read_unlock();
	return match;
}

static int __sched_setscheduler(struct task_struct *p,
				const struct sched_attr *attr,
				bool user)
{
	int newprio = dl_policy(attr->sched_policy) ? MAX_DL_PRIO - 1 :
		      MAX_RT_PRIO - 1 - attr->sched_priority;
	int retval, oldprio, oldpolicy = -1, queued, running;
	int new_effective_prio, policy = attr->sched_policy;
	unsigned long flags;
	const struct sched_class *prev_class;
	struct rq *rq;
	int reset_on_fork;
	int queue_flags = DEQUEUE_SAVE | DEQUEUE_MOVE;

	/* may grab non-irq protected spin_locks */
	BUG_ON(in_interrupt());
recheck:
	/* double check policy once rq lock held */
	if (policy < 0) {
		reset_on_fork = p->sched_reset_on_fork;
		policy = oldpolicy = p->policy;
	} else {
		reset_on_fork = !!(attr->sched_flags & SCHED_FLAG_RESET_ON_FORK);

		if (policy != SCHED_DEADLINE &&
				policy != SCHED_FIFO && policy != SCHED_RR &&
				policy != SCHED_NORMAL && policy != SCHED_BATCH &&
				policy != SCHED_IDLE)
			return -EINVAL;
	}

	if (attr->sched_flags & ~(SCHED_FLAG_RESET_ON_FORK))
		return -EINVAL;

	/*
	 * Valid priorities for SCHED_FIFO and SCHED_RR are
	 * 1..MAX_USER_RT_PRIO-1, valid priority for SCHED_NORMAL,
	 * SCHED_BATCH and SCHED_IDLE is 0.
	 */
	if ((p->mm && attr->sched_priority > MAX_USER_RT_PRIO-1) ||
	    (!p->mm && attr->sched_priority > MAX_RT_PRIO-1))
		return -EINVAL;
	if ((dl_policy(policy) && !__checkparam_dl(attr)) ||
	    (rt_policy(policy) != (attr->sched_priority != 0)))
		return -EINVAL;

	/*
	 * Allow unprivileged RT tasks to decrease priority:
	 */
	if (user && !capable(CAP_SYS_NICE)) {
		if (fair_policy(policy)) {
			if (attr->sched_nice < task_nice(p) &&
			    !can_nice(p, attr->sched_nice))
				return -EPERM;
		}

		if (rt_policy(policy)) {
			unsigned long rlim_rtprio =
					task_rlimit(p, RLIMIT_RTPRIO);

			/* can't set/change the rt policy */
			if (policy != p->policy && !rlim_rtprio)
				return -EPERM;

			/* can't increase priority */
			if (attr->sched_priority > p->rt_priority &&
			    attr->sched_priority > rlim_rtprio)
				return -EPERM;
		}

		 /*
		  * Can't set/change SCHED_DEADLINE policy at all for now
		  * (safest behavior); in the future we would like to allow
		  * unprivileged DL tasks to increase their relative deadline
		  * or reduce their runtime (both ways reducing utilization)
		  */
		if (dl_policy(policy))
			return -EPERM;

		/*
		 * Treat SCHED_IDLE as nice 20. Only allow a switch to
		 * SCHED_NORMAL if the RLIMIT_NICE would normally permit it.
		 */
		if (p->policy == SCHED_IDLE && policy != SCHED_IDLE) {
			if (!can_nice(p, task_nice(p)))
				return -EPERM;
		}

		/* can't change other user's priorities */
		if (!check_same_owner(p))
			return -EPERM;

		/* Normal users shall not reset the sched_reset_on_fork flag */
		if (p->sched_reset_on_fork && !reset_on_fork)
			return -EPERM;
	}

	if (user) {
		retval = security_task_setscheduler(p);
		if (retval)
			return retval;
	}

	/*
	 * make sure no PI-waiters arrive (or leave) while we are
	 * changing the priority of the task:
	 *
	 * To be able to change p->policy safely, the appropriate
	 * runqueue lock must be held.
	 */
	rq = task_rq_lock(p, &flags);

	/*
	 * Changing the policy of the stop threads its a very bad idea
	 */
	if (p == rq->stop) {
		task_rq_unlock(rq, p, &flags);
		return -EINVAL;
	}

	/*
	 * If not changing anything there's no need to proceed further,
	 * but store a possible modification of reset_on_fork.
	 */
	if (unlikely(policy == p->policy)) {
		if (fair_policy(policy) && attr->sched_nice != task_nice(p))
			goto change;
		if (rt_policy(policy) && attr->sched_priority != p->rt_priority)
			goto change;
		if (dl_policy(policy))
			goto change;

		p->sched_reset_on_fork = reset_on_fork;
		task_rq_unlock(rq, p, &flags);
		return 0;
	}
change:

	if (user) {
#ifdef CONFIG_RT_GROUP_SCHED
		/*
		 * Do not allow realtime tasks into groups that have no runtime
		 * assigned.
		 */
		if (rt_bandwidth_enabled() && rt_policy(policy) &&
				task_group(p)->rt_bandwidth.rt_runtime == 0 &&
				!task_group_is_autogroup(task_group(p))) {
			task_rq_unlock(rq, p, &flags);
			return -EPERM;
		}
#endif
#ifdef CONFIG_SMP
		if (dl_bandwidth_enabled() && dl_policy(policy)) {
			cpumask_t *span = rq->rd->span;

			/*
			 * Don't allow tasks with an affinity mask smaller than
			 * the entire root_domain to become SCHED_DEADLINE. We
			 * will also fail if there's no bandwidth available.
			 */
			if (!cpumask_subset(span, &p->cpus_allowed) ||
			    rq->rd->dl_bw.bw == 0) {
				task_rq_unlock(rq, p, &flags);
				return -EPERM;
			}
		}
#endif
	}

	/* recheck policy now with rq lock held */
	if (unlikely(oldpolicy != -1 && oldpolicy != p->policy)) {
		policy = oldpolicy = -1;
		task_rq_unlock(rq, p, &flags);
		goto recheck;
	}

	/*
	 * If setscheduling to SCHED_DEADLINE (or changing the parameters
	 * of a SCHED_DEADLINE task) we need to check if enough bandwidth
	 * is available.
	 */
	if ((dl_policy(policy) || dl_task(p)) && dl_overflow(p, policy, attr)) {
		task_rq_unlock(rq, p, &flags);
		return -EBUSY;
	}

	p->sched_reset_on_fork = reset_on_fork;
	oldprio = p->prio;

	/*
	 * Take priority boosted tasks into account. If the new
	 * effective priority is unchanged, we just store the new
	 * normal parameters and do not touch the scheduler class and
	 * the runqueue. This will be done when the task deboost
	 * itself.
	 */
	new_effective_prio = rt_mutex_get_effective_prio(p, newprio);
	if (new_effective_prio == oldprio)
		queue_flags &= ~DEQUEUE_MOVE;

	queued = task_on_rq_queued(p);
	running = task_current(rq, p);
	if (queued)
		dequeue_task(rq, p, queue_flags);
	if (running)
		put_prev_task(rq, p);

	prev_class = p->sched_class;
	__setscheduler(rq, p, attr, true);

	if (running)
		p->sched_class->set_curr_task(rq);
	if (queued) {
		/*
		 * We enqueue to tail when the priority of a task is
		 * increased (user space view).
		 */
		if (oldprio < p->prio)
			queue_flags |= ENQUEUE_HEAD;

		enqueue_task(rq, p, queue_flags);
	}

	check_class_changed(rq, p, prev_class, oldprio);
	task_rq_unlock(rq, p, &flags);

	rt_mutex_adjust_pi(p);

	return 0;
}

static int _sched_setscheduler(struct task_struct *p, int policy,
			       const struct sched_param *param, bool check)
{
	struct sched_attr attr = {
		.sched_policy   = policy,
		.sched_priority = param->sched_priority,
		.sched_nice	= PRIO_TO_NICE(p->static_prio),
	};

	/* Fixup the legacy SCHED_RESET_ON_FORK hack. */
	if ((policy != SETPARAM_POLICY) && (policy & SCHED_RESET_ON_FORK)) {
		attr.sched_flags |= SCHED_FLAG_RESET_ON_FORK;
		policy &= ~SCHED_RESET_ON_FORK;
		attr.sched_policy = policy;
	}

	return __sched_setscheduler(p, &attr, check);
}
/**
 * sched_setscheduler - change the scheduling policy and/or RT priority of a thread.
 * @p: the task in question.
 * @policy: new policy.
 * @param: structure containing the new RT priority.
 *
 * Return: 0 on success. An error code otherwise.
 *
 * NOTE that the task may be already dead.
 */
int sched_setscheduler(struct task_struct *p, int policy,
		       const struct sched_param *param)
{
	return _sched_setscheduler(p, policy, param, true);
}
EXPORT_SYMBOL_GPL(sched_setscheduler);

int sched_setattr(struct task_struct *p, const struct sched_attr *attr)
{
	return __sched_setscheduler(p, attr, true);
}
EXPORT_SYMBOL_GPL(sched_setattr);

/**
 * sched_setscheduler_nocheck - change the scheduling policy and/or RT priority of a thread from kernelspace.
 * @p: the task in question.
 * @policy: new policy.
 * @param: structure containing the new RT priority.
 *
 * Just like sched_setscheduler, only don't bother checking if the
 * current context has permission.  For example, this is needed in
 * stop_machine(): we create temporary high priority worker threads,
 * but our caller might not have that capability.
 *
 * Return: 0 on success. An error code otherwise.
 */
int sched_setscheduler_nocheck(struct task_struct *p, int policy,
			       const struct sched_param *param)
{
	return _sched_setscheduler(p, policy, param, false);
}
EXPORT_SYMBOL(sched_setscheduler_nocheck);

static int
do_sched_setscheduler(pid_t pid, int policy, struct sched_param __user *param)
{
	struct sched_param lparam;
	struct task_struct *p;
	int retval;

	if (!param || pid < 0)
		return -EINVAL;
	if (copy_from_user(&lparam, param, sizeof(struct sched_param)))
		return -EFAULT;

	rcu_read_lock();
	retval = -ESRCH;
	p = find_process_by_pid(pid);
	if (p != NULL)
		retval = sched_setscheduler(p, policy, &lparam);
	rcu_read_unlock();

	return retval;
}

/*
 * Mimics kernel/events/core.c perf_copy_attr().
 */
static int sched_copy_attr(struct sched_attr __user *uattr,
			   struct sched_attr *attr)
{
	u32 size;
	int ret;

	if (!access_ok(VERIFY_WRITE, uattr, SCHED_ATTR_SIZE_VER0))
		return -EFAULT;

	/*
	 * zero the full structure, so that a short copy will be nice.
	 */
	memset(attr, 0, sizeof(*attr));

	ret = get_user(size, &uattr->size);
	if (ret)
		return ret;

	if (size > PAGE_SIZE)	/* silly large */
		goto err_size;

	if (!size)		/* abi compat */
		size = SCHED_ATTR_SIZE_VER0;

	if (size < SCHED_ATTR_SIZE_VER0)
		goto err_size;

	/*
	 * If we're handed a bigger struct than we know of,
	 * ensure all the unknown bits are 0 - i.e. new
	 * user-space does not rely on any kernel feature
	 * extensions we dont know about yet.
	 */
	if (size > sizeof(*attr)) {
		unsigned char __user *addr;
		unsigned char __user *end;
		unsigned char val;

		addr = (void __user *)uattr + sizeof(*attr);
		end  = (void __user *)uattr + size;

		for (; addr < end; addr++) {
			ret = get_user(val, addr);
			if (ret)
				return ret;
			if (val)
				goto err_size;
		}
		size = sizeof(*attr);
	}

	ret = copy_from_user(attr, uattr, size);
	if (ret)
		return -EFAULT;

	/*
	 * XXX: do we want to be lenient like existing syscalls; or do we want
	 * to be strict and return an error on out-of-bounds values?
	 */
	attr->sched_nice = clamp(attr->sched_nice, MIN_NICE, MAX_NICE);

	return 0;

err_size:
	put_user(sizeof(*attr), &uattr->size);
	return -E2BIG;
}

/**
 * sys_sched_setscheduler - set/change the scheduler policy and RT priority
 * @pid: the pid in question.
 * @policy: new policy.
 * @param: structure containing the new RT priority.
 *
 * Return: 0 on success. An error code otherwise.
 */
SYSCALL_DEFINE3(sched_setscheduler, pid_t, pid, int, policy,
		struct sched_param __user *, param)
{
	/* negative values for policy are not valid */
	if (policy < 0)
		return -EINVAL;

	return do_sched_setscheduler(pid, policy, param);
}

/**
 * sys_sched_setparam - set/change the RT priority of a thread
 * @pid: the pid in question.
 * @param: structure containing the new RT priority.
 *
 * Return: 0 on success. An error code otherwise.
 */
SYSCALL_DEFINE2(sched_setparam, pid_t, pid, struct sched_param __user *, param)
{
	return do_sched_setscheduler(pid, SETPARAM_POLICY, param);
}

/**
 * sys_sched_setattr - same as above, but with extended sched_attr
 * @pid: the pid in question.
 * @uattr: structure containing the extended parameters.
 * @flags: for future extension.
 */
SYSCALL_DEFINE3(sched_setattr, pid_t, pid, struct sched_attr __user *, uattr,
			       unsigned int, flags)
{
	struct sched_attr attr;
	struct task_struct *p;
	int retval;

	if (!uattr || pid < 0 || flags)
		return -EINVAL;

	retval = sched_copy_attr(uattr, &attr);
	if (retval)
		return retval;

	if ((int)attr.sched_policy < 0)
		return -EINVAL;

	rcu_read_lock();
	retval = -ESRCH;
	p = find_process_by_pid(pid);
	if (p != NULL)
		retval = sched_setattr(p, &attr);
	rcu_read_unlock();

	return retval;
}

/**
 * sys_sched_getscheduler - get the policy (scheduling class) of a thread
 * @pid: the pid in question.
 *
 * Return: On success, the policy of the thread. Otherwise, a negative error
 * code.
 */
SYSCALL_DEFINE1(sched_getscheduler, pid_t, pid)
{
	struct task_struct *p;
	int retval;

	if (pid < 0)
		return -EINVAL;

	retval = -ESRCH;
	rcu_read_lock();
	p = find_process_by_pid(pid);
	if (p) {
		retval = security_task_getscheduler(p);
		if (!retval)
			retval = p->policy
				| (p->sched_reset_on_fork ? SCHED_RESET_ON_FORK : 0);
	}
	rcu_read_unlock();
	return retval;
}

/**
 * sys_sched_getparam - get the RT priority of a thread
 * @pid: the pid in question.
 * @param: structure containing the RT priority.
 *
 * Return: On success, 0 and the RT priority is in @param. Otherwise, an error
 * code.
 */
SYSCALL_DEFINE2(sched_getparam, pid_t, pid, struct sched_param __user *, param)
{
	struct sched_param lp = { .sched_priority = 0 };
	struct task_struct *p;
	int retval;

	if (!param || pid < 0)
		return -EINVAL;

	rcu_read_lock();
	p = find_process_by_pid(pid);
	retval = -ESRCH;
	if (!p)
		goto out_unlock;

	retval = security_task_getscheduler(p);
	if (retval)
		goto out_unlock;

	if (task_has_rt_policy(p))
		lp.sched_priority = p->rt_priority;
	rcu_read_unlock();

	/*
	 * This one might sleep, we cannot do it with a spinlock held ...
	 */
	retval = copy_to_user(param, &lp, sizeof(*param)) ? -EFAULT : 0;

	return retval;

out_unlock:
	rcu_read_unlock();
	return retval;
}

static int sched_read_attr(struct sched_attr __user *uattr,
			   struct sched_attr *attr,
			   unsigned int usize)
{
	int ret;

	if (!access_ok(VERIFY_WRITE, uattr, usize))
		return -EFAULT;

	/*
	 * If we're handed a smaller struct than we know of,
	 * ensure all the unknown bits are 0 - i.e. old
	 * user-space does not get uncomplete information.
	 */
	if (usize < sizeof(*attr)) {
		unsigned char *addr;
		unsigned char *end;

		addr = (void *)attr + usize;
		end  = (void *)attr + sizeof(*attr);

		for (; addr < end; addr++) {
			if (*addr)
				return -EFBIG;
		}

		attr->size = usize;
	}

	ret = copy_to_user(uattr, attr, attr->size);
	if (ret)
		return -EFAULT;

	return 0;
}

/**
 * sys_sched_getattr - similar to sched_getparam, but with sched_attr
 * @pid: the pid in question.
 * @uattr: structure containing the extended parameters.
 * @size: sizeof(attr) for fwd/bwd comp.
 * @flags: for future extension.
 */
SYSCALL_DEFINE4(sched_getattr, pid_t, pid, struct sched_attr __user *, uattr,
		unsigned int, size, unsigned int, flags)
{
	struct sched_attr attr = {
		.size = sizeof(struct sched_attr),
	};
	struct task_struct *p;
	int retval;

	if (!uattr || pid < 0 || size > PAGE_SIZE ||
	    size < SCHED_ATTR_SIZE_VER0 || flags)
		return -EINVAL;

	rcu_read_lock();
	p = find_process_by_pid(pid);
	retval = -ESRCH;
	if (!p)
		goto out_unlock;

	retval = security_task_getscheduler(p);
	if (retval)
		goto out_unlock;

	attr.sched_policy = p->policy;
	if (p->sched_reset_on_fork)
		attr.sched_flags |= SCHED_FLAG_RESET_ON_FORK;
	if (task_has_dl_policy(p))
		__getparam_dl(p, &attr);
	else if (task_has_rt_policy(p))
		attr.sched_priority = p->rt_priority;
	else
		attr.sched_nice = task_nice(p);

	rcu_read_unlock();

	retval = sched_read_attr(uattr, &attr, size);
	return retval;

out_unlock:
	rcu_read_unlock();
	return retval;
}

long sched_setaffinity(pid_t pid, const struct cpumask *in_mask)
{
	cpumask_var_t cpus_allowed, new_mask;
	struct task_struct *p;
	int retval;

	rcu_read_lock();

	p = find_process_by_pid(pid);
	if (!p) {
		rcu_read_unlock();
		return -ESRCH;
	}

	/* Prevent p going away */
	get_task_struct(p);
	rcu_read_unlock();

	if (p->flags & PF_NO_SETAFFINITY) {
		retval = -EINVAL;
		goto out_put_task;
	}
	if (!alloc_cpumask_var(&cpus_allowed, GFP_KERNEL)) {
		retval = -ENOMEM;
		goto out_put_task;
	}
	if (!alloc_cpumask_var(&new_mask, GFP_KERNEL)) {
		retval = -ENOMEM;
		goto out_free_cpus_allowed;
	}
	retval = -EPERM;
	if (!check_same_owner(p)) {
		rcu_read_lock();
		if (!ns_capable(__task_cred(p)->user_ns, CAP_SYS_NICE)) {
			rcu_read_unlock();
			goto out_free_new_mask;
		}
		rcu_read_unlock();
	}

	retval = security_task_setscheduler(p);
	if (retval)
		goto out_free_new_mask;

	cpuset_cpus_allowed(p, cpus_allowed);
	cpumask_and(new_mask, in_mask, cpus_allowed);

	/*
	 * Since bandwidth control happens on root_domain basis,
	 * if admission test is enabled, we only admit -deadline
	 * tasks allowed to run on all the CPUs in the task's
	 * root_domain.
	 */
#ifdef CONFIG_SMP
	if (task_has_dl_policy(p) && dl_bandwidth_enabled()) {
		rcu_read_lock();
		if (!cpumask_subset(task_rq(p)->rd->span, new_mask)) {
			retval = -EBUSY;
			rcu_read_unlock();
			goto out_free_new_mask;
		}
		rcu_read_unlock();
	}
#endif
again:
	retval = set_cpus_allowed_ptr(p, new_mask);

	if (!retval) {
		cpuset_cpus_allowed(p, cpus_allowed);
		if (!cpumask_subset(new_mask, cpus_allowed)) {
			/*
			 * We must have raced with a concurrent cpuset
			 * update. Just reset the cpus_allowed to the
			 * cpuset's cpus_allowed
			 */
			cpumask_copy(new_mask, cpus_allowed);
			goto again;
		}
	}
out_free_new_mask:
	free_cpumask_var(new_mask);
out_free_cpus_allowed:
	free_cpumask_var(cpus_allowed);
out_put_task:
	put_task_struct(p);
	return retval;
}

static int get_user_cpu_mask(unsigned long __user *user_mask_ptr, unsigned len,
			     struct cpumask *new_mask)
{
	if (len < cpumask_size())
		cpumask_clear(new_mask);
	else if (len > cpumask_size())
		len = cpumask_size();

	return copy_from_user(new_mask, user_mask_ptr, len) ? -EFAULT : 0;
}

/**
 * sys_sched_setaffinity - set the cpu affinity of a process
 * @pid: pid of the process
 * @len: length in bytes of the bitmask pointed to by user_mask_ptr
 * @user_mask_ptr: user-space pointer to the new cpu mask
 *
 * Return: 0 on success. An error code otherwise.
 */
SYSCALL_DEFINE3(sched_setaffinity, pid_t, pid, unsigned int, len,
		unsigned long __user *, user_mask_ptr)
{
	cpumask_var_t new_mask;
	int retval;

	if (!alloc_cpumask_var(&new_mask, GFP_KERNEL))
		return -ENOMEM;

	retval = get_user_cpu_mask(user_mask_ptr, len, new_mask);
	if (retval == 0)
		retval = sched_setaffinity(pid, new_mask);
	free_cpumask_var(new_mask);
	return retval;
}

long sched_getaffinity(pid_t pid, struct cpumask *mask)
{
	struct task_struct *p;
	unsigned long flags;
	int retval;

	rcu_read_lock();

	retval = -ESRCH;
	p = find_process_by_pid(pid);
	if (!p)
		goto out_unlock;

	retval = security_task_getscheduler(p);
	if (retval)
		goto out_unlock;

	raw_spin_lock_irqsave(&p->pi_lock, flags);
	cpumask_and(mask, &p->cpus_allowed, cpu_active_mask);
	raw_spin_unlock_irqrestore(&p->pi_lock, flags);

out_unlock:
	rcu_read_unlock();

	return retval;
}

/**
 * sys_sched_getaffinity - get the cpu affinity of a process
 * @pid: pid of the process
 * @len: length in bytes of the bitmask pointed to by user_mask_ptr
 * @user_mask_ptr: user-space pointer to hold the current cpu mask
 *
 * Return: 0 on success. An error code otherwise.
 */
SYSCALL_DEFINE3(sched_getaffinity, pid_t, pid, unsigned int, len,
		unsigned long __user *, user_mask_ptr)
{
	int ret;
	cpumask_var_t mask;

	if ((len * BITS_PER_BYTE) < nr_cpu_ids)
		return -EINVAL;
	if (len & (sizeof(unsigned long)-1))
		return -EINVAL;

	if (!alloc_cpumask_var(&mask, GFP_KERNEL))
		return -ENOMEM;

	ret = sched_getaffinity(pid, mask);
	if (ret == 0) {
		size_t retlen = min_t(size_t, len, cpumask_size());

		if (copy_to_user(user_mask_ptr, mask, retlen))
			ret = -EFAULT;
		else
			ret = retlen;
	}
	free_cpumask_var(mask);

	return ret;
}

/**
 * sys_sched_yield - yield the current processor to other threads.
 *
 * This function yields the current CPU to other tasks. If there are no
 * other threads running on this CPU then this function will return.
 *
 * Return: 0.
 */
SYSCALL_DEFINE0(sched_yield)
{
	struct rq *rq = this_rq_lock();

	schedstat_inc(rq, yld_count);
	current->sched_class->yield_task(rq);

	/*
	 * Since we are going to call schedule() anyway, there's
	 * no need to preempt or enable interrupts:
	 */
	__release(rq->lock);
	spin_release(&rq->lock.dep_map, 1, _THIS_IP_);
	do_raw_spin_unlock(&rq->lock);
	sched_preempt_enable_no_resched();

	schedule();

	return 0;
}

static void __cond_resched(void)
{
	__preempt_count_add(PREEMPT_ACTIVE);
	__schedule();
	__preempt_count_sub(PREEMPT_ACTIVE);
}

int __sched _cond_resched(void)
{
	if (should_resched(0)) {
		__cond_resched();
		return 1;
	}
	return 0;
}
EXPORT_SYMBOL(_cond_resched);

/*
 * __cond_resched_lock() - if a reschedule is pending, drop the given lock,
 * call schedule, and on return reacquire the lock.
 *
 * This works OK both with and without CONFIG_PREEMPT. We do strange low-level
 * operations here to prevent schedule() from being called twice (once via
 * spin_unlock(), once by hand).
 */
int __cond_resched_lock(spinlock_t *lock)
{
	int resched = should_resched(PREEMPT_LOCK_OFFSET);
	int ret = 0;

	lockdep_assert_held(lock);

	if (spin_needbreak(lock) || resched) {
		spin_unlock(lock);
		if (resched)
			__cond_resched();
		else
			cpu_relax();
		ret = 1;
		spin_lock(lock);
	}
	return ret;
}
EXPORT_SYMBOL(__cond_resched_lock);

int __sched __cond_resched_softirq(void)
{
	BUG_ON(!in_softirq());

	if (should_resched(SOFTIRQ_DISABLE_OFFSET)) {
		local_bh_enable();
		__cond_resched();
		local_bh_disable();
		return 1;
	}
	return 0;
}
EXPORT_SYMBOL(__cond_resched_softirq);

/**
 * yield - yield the current processor to other threads.
 *
 * Do not ever use this function, there's a 99% chance you're doing it wrong.
 *
 * The scheduler is at all times free to pick the calling task as the most
 * eligible task to run, if removing the yield() call from your code breaks
 * it, its already broken.
 *
 * Typical broken usage is:
 *
 * while (!event)
 * 	yield();
 *
 * where one assumes that yield() will let 'the other' process run that will
 * make event true. If the current task is a SCHED_FIFO task that will never
 * happen. Never use yield() as a progress guarantee!!
 *
 * If you want to use yield() to wait for something, use wait_event().
 * If you want to use yield() to be 'nice' for others, use cond_resched().
 * If you still want to use yield(), do not!
 */
void __sched yield(void)
{
	set_current_state(TASK_RUNNING);
	sys_sched_yield();
}
EXPORT_SYMBOL(yield);

/**
 * yield_to - yield the current processor to another thread in
 * your thread group, or accelerate that thread toward the
 * processor it's on.
 * @p: target task
 * @preempt: whether task preemption is allowed or not
 *
 * It's the caller's job to ensure that the target task struct
 * can't go away on us before we can do any checks.
 *
 * Return:
 *	true (>0) if we indeed boosted the target task.
 *	false (0) if we failed to boost the target.
 *	-ESRCH if there's no task to yield to.
 */
int __sched yield_to(struct task_struct *p, bool preempt)
{
	struct task_struct *curr = current;
	struct rq *rq, *p_rq;
	unsigned long flags;
	int yielded = 0;

	local_irq_save(flags);
	rq = this_rq();

again:
	p_rq = task_rq(p);
	/*
	 * If we're the only runnable task on the rq and target rq also
	 * has only one task, there's absolutely no point in yielding.
	 */
	if (rq->nr_running == 1 && p_rq->nr_running == 1) {
		yielded = -ESRCH;
		goto out_irq;
	}

	double_rq_lock(rq, p_rq);
	if (task_rq(p) != p_rq) {
		double_rq_unlock(rq, p_rq);
		goto again;
	}

	if (!curr->sched_class->yield_to_task)
		goto out_unlock;

	if (curr->sched_class != p->sched_class)
		goto out_unlock;

	if (task_running(p_rq, p) || p->state)
		goto out_unlock;

	yielded = curr->sched_class->yield_to_task(rq, p, preempt);
	if (yielded) {
		schedstat_inc(rq, yld_count);
		/*
		 * Make p's CPU reschedule; pick_next_entity takes care of
		 * fairness.
		 */
		if (preempt && rq != p_rq)
			resched_curr(p_rq);
	}

out_unlock:
	double_rq_unlock(rq, p_rq);
out_irq:
	local_irq_restore(flags);

	if (yielded > 0)
		schedule();

	return yielded;
}
EXPORT_SYMBOL_GPL(yield_to);

/*
 * This task is about to go to sleep on IO. Increment rq->nr_iowait so
 * that process accounting knows that this is a task in IO wait state.
 */
void __sched io_schedule(void)
{
	struct rq *rq = raw_rq();

	delayacct_blkio_start();
	atomic_inc(&rq->nr_iowait);
	blk_flush_plug(current);
	current->in_iowait = 1;
	schedule();
	current->in_iowait = 0;
	atomic_dec(&rq->nr_iowait);
	delayacct_blkio_end();
}
EXPORT_SYMBOL(io_schedule);

long __sched io_schedule_timeout(long timeout)
{
	struct rq *rq = raw_rq();
	long ret;

	delayacct_blkio_start();
	atomic_inc(&rq->nr_iowait);
	blk_flush_plug(current);
	current->in_iowait = 1;
	ret = schedule_timeout(timeout);
	current->in_iowait = 0;
	atomic_dec(&rq->nr_iowait);
	delayacct_blkio_end();
	return ret;
}
EXPORT_SYMBOL(io_schedule_timeout);

/**
 * sys_sched_get_priority_max - return maximum RT priority.
 * @policy: scheduling class.
 *
 * Return: On success, this syscall returns the maximum
 * rt_priority that can be used by a given scheduling class.
 * On failure, a negative error code is returned.
 */
SYSCALL_DEFINE1(sched_get_priority_max, int, policy)
{
	int ret = -EINVAL;

	switch (policy) {
	case SCHED_FIFO:
	case SCHED_RR:
		ret = MAX_USER_RT_PRIO-1;
		break;
	case SCHED_DEADLINE:
	case SCHED_NORMAL:
	case SCHED_BATCH:
	case SCHED_IDLE:
		ret = 0;
		break;
	}
	return ret;
}

/**
 * sys_sched_get_priority_min - return minimum RT priority.
 * @policy: scheduling class.
 *
 * Return: On success, this syscall returns the minimum
 * rt_priority that can be used by a given scheduling class.
 * On failure, a negative error code is returned.
 */
SYSCALL_DEFINE1(sched_get_priority_min, int, policy)
{
	int ret = -EINVAL;

	switch (policy) {
	case SCHED_FIFO:
	case SCHED_RR:
		ret = 1;
		break;
	case SCHED_DEADLINE:
	case SCHED_NORMAL:
	case SCHED_BATCH:
	case SCHED_IDLE:
		ret = 0;
	}
	return ret;
}

/**
 * sys_sched_rr_get_interval - return the default timeslice of a process.
 * @pid: pid of the process.
 * @interval: userspace pointer to the timeslice value.
 *
 * this syscall writes the default timeslice value of a given process
 * into the user-space timespec buffer. A value of '0' means infinity.
 *
 * Return: On success, 0 and the timeslice is in @interval. Otherwise,
 * an error code.
 */
SYSCALL_DEFINE2(sched_rr_get_interval, pid_t, pid,
		struct timespec __user *, interval)
{
	struct task_struct *p;
	unsigned int time_slice;
	unsigned long flags;
	struct rq *rq;
	int retval;
	struct timespec t;

	if (pid < 0)
		return -EINVAL;

	retval = -ESRCH;
	rcu_read_lock();
	p = find_process_by_pid(pid);
	if (!p)
		goto out_unlock;

	retval = security_task_getscheduler(p);
	if (retval)
		goto out_unlock;

	rq = task_rq_lock(p, &flags);
	time_slice = 0;
	if (p->sched_class->get_rr_interval)
		time_slice = p->sched_class->get_rr_interval(rq, p);
	task_rq_unlock(rq, p, &flags);

	rcu_read_unlock();
	jiffies_to_timespec(time_slice, &t);
	retval = copy_to_user(interval, &t, sizeof(t)) ? -EFAULT : 0;
	return retval;

out_unlock:
	rcu_read_unlock();
	return retval;
}

static const char stat_nam[] = TASK_STATE_TO_CHAR_STR;

void sched_show_task(struct task_struct *p)
{
	unsigned long free = 0;
	int ppid;
	unsigned state;

	state = p->state ? __ffs(p->state) + 1 : 0;
	printk(KERN_INFO "%-15.15s %c", p->comm,
		state < sizeof(stat_nam) - 1 ? stat_nam[state] : '?');
#if BITS_PER_LONG == 32
	if (state == TASK_RUNNING)
		printk(KERN_CONT " running  ");
	else
		printk(KERN_CONT " %08lx ", thread_saved_pc(p));
#else
	if (state == TASK_RUNNING)
		printk(KERN_CONT "  running task    ");
	else
		printk(KERN_CONT " %016lx ", thread_saved_pc(p));
#endif
#ifdef CONFIG_DEBUG_STACK_USAGE
	free = stack_not_used(p);
#endif
	rcu_read_lock();
	ppid = task_pid_nr(rcu_dereference(p->real_parent));
	rcu_read_unlock();
	printk(KERN_CONT "%5lu %5d %6d 0x%08lx\n", free,
		task_pid_nr(p), ppid,
		(unsigned long)task_thread_info(p)->flags);

	print_worker_info(KERN_INFO, p);
	show_stack(p, NULL);
}

void show_state_filter(unsigned long state_filter)
{
	struct task_struct *g, *p;

#if BITS_PER_LONG == 32
	printk(KERN_INFO
		"  task                PC stack   pid father\n");
#else
	printk(KERN_INFO
		"  task                        PC stack   pid father\n");
#endif
	rcu_read_lock();
	for_each_process_thread(g, p) {
		/*
		 * reset the NMI-timeout, listing all files on a slow
		 * console might take a lot of time:
		 */
		touch_nmi_watchdog();
		if (!state_filter || (p->state & state_filter))
			sched_show_task(p);
	}

	touch_all_softlockup_watchdogs();

#ifdef CONFIG_SYSRQ_SCHED_DEBUG
	sysrq_sched_debug_show();
#endif
	rcu_read_unlock();
	/*
	 * Only show locks if all tasks are dumped:
	 */
	if (!state_filter)
		debug_show_all_locks();
}

void init_idle_bootup_task(struct task_struct *idle)
{
	idle->sched_class = &idle_sched_class;
}

/**
 * init_idle - set up an idle thread for a given CPU
 * @idle: task in question
 * @cpu: cpu the idle task belongs to
 *
 * NOTE: this function does not set the idle thread's NEED_RESCHED
 * flag, to make booting more robust.
 */
void init_idle(struct task_struct *idle, int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	unsigned long flags;

	__sched_fork(0, idle);

	raw_spin_lock_irqsave(&rq->lock, flags);

	idle->state = TASK_RUNNING;
	idle->se.exec_start = sched_clock();

	do_set_cpus_allowed(idle, cpumask_of(cpu));
	/*
	 * We're having a chicken and egg problem, even though we are
	 * holding rq->lock, the cpu isn't yet set to this cpu so the
	 * lockdep check in task_group() will fail.
	 *
	 * Similar case to sched_fork(). / Alternatively we could
	 * use task_rq_lock() here and obtain the other rq->lock.
	 *
	 * Silence PROVE_RCU
	 */
	rcu_read_lock();
	__set_task_cpu(idle, cpu);
	rcu_read_unlock();

	rq->curr = rq->idle = idle;
	idle->on_rq = TASK_ON_RQ_QUEUED;
#if defined(CONFIG_SMP)
	idle->on_cpu = 1;
#endif
	raw_spin_unlock_irqrestore(&rq->lock, flags);

	/* Set the preempt count _outside_ the spinlocks! */
	init_idle_preempt_count(idle, cpu);

	/*
	 * The idle tasks have their own, simple scheduling class:
	 */
	idle->sched_class = &idle_sched_class;
	ftrace_graph_init_idle_task(idle, cpu);
	vtime_init_idle(idle, cpu);
#if defined(CONFIG_SMP)
	sprintf(idle->comm, "%s/%d", INIT_TASK_COMM, cpu);
#endif
}

#ifdef CONFIG_SMP
/*
 * move_queued_task - move a queued task to new rq.
 *
 * Returns (locked) new rq. Old rq's lock is released.
 */
static struct rq *move_queued_task(struct task_struct *p, int new_cpu)
{
	struct rq *rq = task_rq(p);

	lockdep_assert_held(&rq->lock);

	dequeue_task(rq, p, DEQUEUE_MIGRATING);
	p->on_rq = TASK_ON_RQ_MIGRATING;
	double_lock_balance(rq, cpu_rq(new_cpu));
	set_task_cpu(p, new_cpu);
	double_unlock_balance(rq, cpu_rq(new_cpu));
	raw_spin_unlock(&rq->lock);

	rq = cpu_rq(new_cpu);

	raw_spin_lock(&rq->lock);
	BUG_ON(task_cpu(p) != new_cpu);
	p->on_rq = TASK_ON_RQ_QUEUED;
	enqueue_task(rq, p, ENQUEUE_MIGRATING);
	check_preempt_curr(rq, p, 0);

	return rq;
}

void do_set_cpus_allowed(struct task_struct *p, const struct cpumask *new_mask)
{
	if (p->sched_class && p->sched_class->set_cpus_allowed)
		p->sched_class->set_cpus_allowed(p, new_mask);

	cpumask_copy(&p->cpus_allowed, new_mask);
	p->nr_cpus_allowed = cpumask_weight(new_mask);
}

/*
 * This is how migration works:
 *
 * 1) we invoke migration_cpu_stop() on the target CPU using
 *    stop_one_cpu().
 * 2) stopper starts to run (implicitly forcing the migrated thread
 *    off the CPU)
 * 3) it checks whether the migrated task is still in the wrong runqueue.
 * 4) if it's in the wrong runqueue then the migration thread removes
 *    it and puts it into the right queue.
 * 5) stopper completes and stop_one_cpu() returns and the migration
 *    is done.
 */

/*
 * Change a given task's CPU affinity. Migrate the thread to a
 * proper CPU and schedule it away if the CPU it's executing on
 * is removed from the allowed bitmask.
 *
 * NOTE: the caller must have a valid reference to the task, the
 * task must not exit() & deallocate itself prematurely. The
 * call is not atomic; no spinlocks may be held.
 */
int set_cpus_allowed_ptr(struct task_struct *p, const struct cpumask *new_mask)
{
	unsigned long flags;
	struct rq *rq;
	unsigned int dest_cpu;
	int ret = 0;

	rq = task_rq_lock(p, &flags);

	if (cpumask_equal(&p->cpus_allowed, new_mask))
		goto out;

	dest_cpu = cpumask_any_and(cpu_active_mask, new_mask);
	if (dest_cpu >= nr_cpu_ids) {
		ret = -EINVAL;
		goto out;
	}

	do_set_cpus_allowed(p, new_mask);

	/* Can the task run on the task's current CPU? If so, we're done */
	if (cpumask_test_cpu(task_cpu(p), new_mask))
		goto out;

	if (task_running(rq, p) || p->state == TASK_WAKING) {
		struct migration_arg arg = { p, dest_cpu };
		/* Need help from migration thread: drop lock and wait. */
		task_rq_unlock(rq, p, &flags);
		stop_one_cpu(cpu_of(rq), migration_cpu_stop, &arg);
		tlb_migrate_finish(p->mm);
		return 0;
	} else if (task_on_rq_queued(p))
		rq = move_queued_task(p, dest_cpu);
out:
	task_rq_unlock(rq, p, &flags);

	return ret;
}
EXPORT_SYMBOL_GPL(set_cpus_allowed_ptr);

/*
 * Move (not current) task off this cpu, onto dest cpu. We're doing
 * this because either it can't run here any more (set_cpus_allowed()
 * away from this CPU, or CPU going down), or because we're
 * attempting to rebalance this task on exec (sched_exec).
 *
 * So we race with normal scheduler movements, but that's OK, as long
 * as the task is no longer on this CPU.
 *
 * Returns non-zero if task was successfully migrated.
 */
static int __migrate_task(struct task_struct *p, int src_cpu, int dest_cpu)
{
	struct rq *rq;
	bool moved = false;
	int ret = 0;
	int check_groups;

	if (unlikely(!cpu_active(dest_cpu)))
		return ret;

	rq = cpu_rq(src_cpu);

	raw_spin_lock(&p->pi_lock);
	raw_spin_lock(&rq->lock);
	/* Already moved. */
	if (task_cpu(p) != src_cpu)
		goto done;

	/* Affinity changed (again). */
	if (!cpumask_test_cpu(dest_cpu, tsk_cpus_allowed(p)))
		goto fail;

	/* No need for rcu_read_lock() here. Protected by pi->lock */
	check_groups = is_task_in_related_thread_group(p);

	/*
	 * If we're not on a rq, the next wake-up will ensure we're
	 * placed properly.
	 */
	if (task_on_rq_queued(p)) {
		rq = move_queued_task(p, dest_cpu);
		moved = true;
	}
done:
	ret = 1;
fail:
	raw_spin_unlock(&rq->lock);
	raw_spin_unlock(&p->pi_lock);
	if (moved && !same_freq_domain(src_cpu, dest_cpu)) {
		check_for_freq_change(cpu_rq(src_cpu), false, check_groups);
		check_for_freq_change(cpu_rq(dest_cpu), false, check_groups);
	} else if (moved) {
		check_for_freq_change(cpu_rq(dest_cpu), true, false);
	}
	if (moved && task_notify_on_migrate(p)) {
		struct migration_notify_data mnd;

		mnd.src_cpu = src_cpu;
		mnd.dest_cpu = dest_cpu;
		mnd.load = pct_task_load(p);
		atomic_notifier_call_chain(&migration_notifier_head,
					   0, (void *)&mnd);
	}
	return ret;
}

#ifdef CONFIG_NUMA_BALANCING
/* Migrate current task p to target_cpu */
int migrate_task_to(struct task_struct *p, int target_cpu)
{
	struct migration_arg arg = { p, target_cpu };
	int curr_cpu = task_cpu(p);

	if (curr_cpu == target_cpu)
		return 0;

	if (!cpumask_test_cpu(target_cpu, tsk_cpus_allowed(p)))
		return -EINVAL;

	/* TODO: This is not properly updating schedstats */

	trace_sched_move_numa(p, curr_cpu, target_cpu);
	return stop_one_cpu(curr_cpu, migration_cpu_stop, &arg);
}

/*
 * Requeue a task on a given node and accurately track the number of NUMA
 * tasks on the runqueues
 */
void sched_setnuma(struct task_struct *p, int nid)
{
	struct rq *rq;
	unsigned long flags;
	bool queued, running;

	rq = task_rq_lock(p, &flags);
	queued = task_on_rq_queued(p);
	running = task_current(rq, p);

	if (queued)
		dequeue_task(rq, p, DEQUEUE_SAVE);
	if (running)
		put_prev_task(rq, p);

	p->numa_preferred_nid = nid;

	if (running)
		p->sched_class->set_curr_task(rq);
	if (queued)
		enqueue_task(rq, p, ENQUEUE_RESTORE);
	task_rq_unlock(rq, p, &flags);
}
#endif

/*
 * migration_cpu_stop - this will be executed by a highprio stopper thread
 * and performs thread migration by bumping thread off CPU then
 * 'pushing' onto another runqueue.
 */
static int migration_cpu_stop(void *data)
{
	struct migration_arg *arg = data;

	/*
	 * The original target cpu might have gone down and we might
	 * be on another cpu but it doesn't matter.
	 */
	local_irq_disable();
	/*
	 * We need to explicitly wake pending tasks before running
	 * __migrate_task() such that we will not miss enforcing cpus_allowed
	 * during wakeups, see set_cpus_allowed_ptr()'s TASK_WAKING test.
	 */
	sched_ttwu_pending();
	__migrate_task(arg->task, raw_smp_processor_id(), arg->dest_cpu);
	local_irq_enable();
	return 0;
}

#ifdef CONFIG_HOTPLUG_CPU

/*
 * Ensures that the idle task is using init_mm right before its cpu goes
 * offline.
 */
void idle_task_exit(void)
{
	struct mm_struct *mm = current->active_mm;

	BUG_ON(cpu_online(smp_processor_id()));

	if (mm != &init_mm) {
		switch_mm(mm, &init_mm, current);
		finish_arch_post_lock_switch();
	}
	mmdrop(mm);
}

/*
 * Since this CPU is going 'away' for a while, fold any nr_active delta
 * we might have. Assumes we're called after migrate_tasks() so that the
 * nr_active count is stable.
 *
 * Also see the comment "Global load-average calculations".
 */
static void calc_load_migrate(struct rq *rq)
{
	long delta = calc_load_fold_active(rq);
	if (delta)
		atomic_long_add(delta, &calc_load_tasks);
}

static void put_prev_task_fake(struct rq *rq, struct task_struct *prev)
{
}

static const struct sched_class fake_sched_class = {
	.put_prev_task = put_prev_task_fake,
};

static struct task_struct fake_task = {
	/*
	 * Avoid pull_{rt,dl}_task()
	 */
	.prio = MAX_PRIO + 1,
	.sched_class = &fake_sched_class,
};

/*
 * Migrate all tasks from the rq, sleeping tasks will be migrated by
 * try_to_wake_up()->select_task_rq().
 *
 * Called with rq->lock held even though we'er in stop_machine() and
 * there's no concurrency possible, we hold the required locks anyway
 * because of lock validation efforts.
 */
static void migrate_tasks(unsigned int dead_cpu)
{
	struct rq *rq = cpu_rq(dead_cpu);
	struct task_struct *next, *stop = rq->stop;
	int dest_cpu;

	/*
	 * Fudge the rq selection such that the below task selection loop
	 * doesn't get stuck on the currently eligible stop task.
	 *
	 * We're currently inside stop_machine() and the rq is either stuck
	 * in the stop_machine_cpu_stop() loop, or we're executing this code,
	 * either way we should never end up calling schedule() until we're
	 * done here.
	 */
	rq->stop = NULL;

	/*
	 * put_prev_task() and pick_next_task() sched
	 * class method both need to have an up-to-date
	 * value of rq->clock[_task]
	 */
	update_rq_clock(rq);

	for ( ; ; ) {
		/*
		 * There's this thread running, bail when that's the only
		 * remaining thread.
		 */
		if (rq->nr_running == 1)
			break;

		next = pick_next_task(rq, &fake_task);
		BUG_ON(!next);
		next->sched_class->put_prev_task(rq, next);

		/* Find suitable destination for @next, with force if needed. */
		dest_cpu = select_fallback_rq(dead_cpu, next);
		raw_spin_unlock(&rq->lock);

		__migrate_task(next, dead_cpu, dest_cpu);

		raw_spin_lock(&rq->lock);
	}

	rq->stop = stop;
}

#endif /* CONFIG_HOTPLUG_CPU */

#if defined(CONFIG_SCHED_DEBUG) && defined(CONFIG_SYSCTL)

static struct ctl_table sd_ctl_dir[] = {
	{
		.procname	= "sched_domain",
		.mode		= 0555,
	},
	{}
};

static struct ctl_table sd_ctl_root[] = {
	{
		.procname	= "kernel",
		.mode		= 0555,
		.child		= sd_ctl_dir,
	},
	{}
};

static struct ctl_table *sd_alloc_ctl_entry(int n)
{
	struct ctl_table *entry =
		kcalloc(n, sizeof(struct ctl_table), GFP_KERNEL);

	return entry;
}

static void sd_free_ctl_entry(struct ctl_table **tablep)
{
	struct ctl_table *entry;

	/*
	 * In the intermediate directories, both the child directory and
	 * procname are dynamically allocated and could fail but the mode
	 * will always be set. In the lowest directory the names are
	 * static strings and all have proc handlers.
	 */
	for (entry = *tablep; entry->mode; entry++) {
		if (entry->child)
			sd_free_ctl_entry(&entry->child);
		if (entry->proc_handler == NULL)
			kfree(entry->procname);
	}

	kfree(*tablep);
	*tablep = NULL;
}

static int min_load_idx = 0;
static int max_load_idx = CPU_LOAD_IDX_MAX-1;

static void
set_table_entry(struct ctl_table *entry,
		const char *procname, void *data, int maxlen,
		umode_t mode, proc_handler *proc_handler,
		bool load_idx)
{
	entry->procname = procname;
	entry->data = data;
	entry->maxlen = maxlen;
	entry->mode = mode;
	entry->proc_handler = proc_handler;

	if (load_idx) {
		entry->extra1 = &min_load_idx;
		entry->extra2 = &max_load_idx;
	}
}

static struct ctl_table *
sd_alloc_ctl_domain_table(struct sched_domain *sd)
{
	struct ctl_table *table = sd_alloc_ctl_entry(14);

	if (table == NULL)
		return NULL;

	set_table_entry(&table[0], "min_interval", &sd->min_interval,
		sizeof(long), 0644, proc_doulongvec_minmax, false);
	set_table_entry(&table[1], "max_interval", &sd->max_interval,
		sizeof(long), 0644, proc_doulongvec_minmax, false);
	set_table_entry(&table[2], "busy_idx", &sd->busy_idx,
		sizeof(int), 0644, proc_dointvec_minmax, true);
	set_table_entry(&table[3], "idle_idx", &sd->idle_idx,
		sizeof(int), 0644, proc_dointvec_minmax, true);
	set_table_entry(&table[4], "newidle_idx", &sd->newidle_idx,
		sizeof(int), 0644, proc_dointvec_minmax, true);
	set_table_entry(&table[5], "wake_idx", &sd->wake_idx,
		sizeof(int), 0644, proc_dointvec_minmax, true);
	set_table_entry(&table[6], "forkexec_idx", &sd->forkexec_idx,
		sizeof(int), 0644, proc_dointvec_minmax, true);
	set_table_entry(&table[7], "busy_factor", &sd->busy_factor,
		sizeof(int), 0644, proc_dointvec_minmax, false);
	set_table_entry(&table[8], "imbalance_pct", &sd->imbalance_pct,
		sizeof(int), 0644, proc_dointvec_minmax, false);
	set_table_entry(&table[9], "cache_nice_tries",
		&sd->cache_nice_tries,
		sizeof(int), 0644, proc_dointvec_minmax, false);
	set_table_entry(&table[10], "flags", &sd->flags,
		sizeof(int), 0644, proc_dointvec_minmax, false);
	set_table_entry(&table[11], "max_newidle_lb_cost",
		&sd->max_newidle_lb_cost,
		sizeof(long), 0644, proc_doulongvec_minmax, false);
	set_table_entry(&table[12], "name", sd->name,
		CORENAME_MAX_SIZE, 0444, proc_dostring, false);
	/* &table[13] is terminator */

	return table;
}

static struct ctl_table *sd_alloc_ctl_cpu_table(int cpu)
{
	struct ctl_table *entry, *table;
	struct sched_domain *sd;
	int domain_num = 0, i;
	char buf[32];

	for_each_domain(cpu, sd)
		domain_num++;
	entry = table = sd_alloc_ctl_entry(domain_num + 1);
	if (table == NULL)
		return NULL;

	i = 0;
	for_each_domain(cpu, sd) {
		snprintf(buf, 32, "domain%d", i);
		entry->procname = kstrdup(buf, GFP_KERNEL);
		entry->mode = 0555;
		entry->child = sd_alloc_ctl_domain_table(sd);
		entry++;
		i++;
	}
	return table;
}

static struct ctl_table_header *sd_sysctl_header;
static void register_sched_domain_sysctl(void)
{
	int i, cpu_num = num_possible_cpus();
	struct ctl_table *entry = sd_alloc_ctl_entry(cpu_num + 1);
	char buf[32];

	WARN_ON(sd_ctl_dir[0].child);
	sd_ctl_dir[0].child = entry;

	if (entry == NULL)
		return;

	for_each_possible_cpu(i) {
		snprintf(buf, 32, "cpu%d", i);
		entry->procname = kstrdup(buf, GFP_KERNEL);
		entry->mode = 0555;
		entry->child = sd_alloc_ctl_cpu_table(i);
		entry++;
	}

	WARN_ON(sd_sysctl_header);
	sd_sysctl_header = register_sysctl_table(sd_ctl_root);
}

/* may be called multiple times per register */
static void unregister_sched_domain_sysctl(void)
{
	if (sd_sysctl_header)
		unregister_sysctl_table(sd_sysctl_header);
	sd_sysctl_header = NULL;
	if (sd_ctl_dir[0].child)
		sd_free_ctl_entry(&sd_ctl_dir[0].child);
}
#else
static void register_sched_domain_sysctl(void)
{
}
static void unregister_sched_domain_sysctl(void)
{
}
#endif

static void set_rq_online(struct rq *rq)
{
	if (!rq->online) {
		const struct sched_class *class;

		cpumask_set_cpu(rq->cpu, rq->rd->online);
		rq->online = 1;

		for_each_class(class) {
			if (class->rq_online)
				class->rq_online(rq);
		}
	}
}

static void set_rq_offline(struct rq *rq)
{
	if (rq->online) {
		const struct sched_class *class;

		for_each_class(class) {
			if (class->rq_offline)
				class->rq_offline(rq);
		}

		cpumask_clear_cpu(rq->cpu, rq->rd->online);
		rq->online = 0;
	}
}

/*
 * migration_call - callback that gets triggered when a CPU is added.
 * Here we can start up the necessary migration thread for the new CPU.
 */
static int
migration_call(struct notifier_block *nfb, unsigned long action, void *hcpu)
{
	int cpu = (long)hcpu;
	unsigned long flags;
	struct rq *rq = cpu_rq(cpu);

	switch (action & ~CPU_TASKS_FROZEN) {

	case CPU_UP_PREPARE:
		raw_spin_lock_irqsave(&rq->lock, flags);
		set_window_start(rq);
		raw_spin_unlock_irqrestore(&rq->lock, flags);
		rq->calc_load_update = calc_load_update;
		break;

	case CPU_ONLINE:
		/* Update our root-domain */
		raw_spin_lock_irqsave(&rq->lock, flags);
		if (rq->rd) {
			BUG_ON(!cpumask_test_cpu(cpu, rq->rd->span));

			set_rq_online(rq);
		}
		raw_spin_unlock_irqrestore(&rq->lock, flags);
		break;

#ifdef CONFIG_HOTPLUG_CPU
	case CPU_DYING:
		sched_ttwu_pending();
		/* Update our root-domain */
		raw_spin_lock_irqsave(&rq->lock, flags);
		migrate_sync_cpu(cpu);

		if (rq->rd) {
			BUG_ON(!cpumask_test_cpu(cpu, rq->rd->span));
			set_rq_offline(rq);
		}
		migrate_tasks(cpu);
		BUG_ON(rq->nr_running != 1); /* the migration thread */
		raw_spin_unlock_irqrestore(&rq->lock, flags);
		break;

	case CPU_DEAD:
		clear_hmp_request(cpu);
		calc_load_migrate(rq);
		break;
#endif
	}

	update_max_interval();

	return NOTIFY_OK;
}

/*
 * Register at high priority so that task migration (migrate_all_tasks)
 * happens before everything else.  This has to be lower priority than
 * the notifier in the perf_event subsystem, though.
 */
static struct notifier_block migration_notifier = {
	.notifier_call = migration_call,
	.priority = CPU_PRI_MIGRATION,
};

static void __cpuinit set_cpu_rq_start_time(void)
{
	int cpu = smp_processor_id();
	struct rq *rq = cpu_rq(cpu);
	rq->age_stamp = sched_clock_cpu(cpu);
}

static int sched_cpu_active(struct notifier_block *nfb,
				      unsigned long action, void *hcpu)
{
	switch (action & ~CPU_TASKS_FROZEN) {
	case CPU_STARTING:
		set_cpu_rq_start_time();
		return NOTIFY_OK;
	case CPU_ONLINE:
		/*
		 * At this point a starting CPU has marked itself as online via
		 * set_cpu_online(). But it might not yet have marked itself
		 * as active, which is essential from here on.
		 *
		 * Thus, fall-through and help the starting CPU along.
		 */
	case CPU_DOWN_FAILED:
		set_cpu_active((long)hcpu, true);
		return NOTIFY_OK;
	default:
		return NOTIFY_DONE;
	}
}

static int sched_cpu_inactive(struct notifier_block *nfb,
					unsigned long action, void *hcpu)
{
	unsigned long flags;
	long cpu = (long)hcpu;
	struct dl_bw *dl_b;

	switch (action & ~CPU_TASKS_FROZEN) {
	case CPU_DOWN_PREPARE:
		set_cpu_active(cpu, false);

		/* explicitly allow suspend */
		if (!(action & CPU_TASKS_FROZEN)) {
			bool overflow;
			int cpus;

			rcu_read_lock_sched();
			dl_b = dl_bw_of(cpu);

			raw_spin_lock_irqsave(&dl_b->lock, flags);
			cpus = dl_bw_cpus(cpu);
			overflow = __dl_overflow(dl_b, cpus, 0, 0);
			raw_spin_unlock_irqrestore(&dl_b->lock, flags);

			rcu_read_unlock_sched();

			if (overflow)
				return notifier_from_errno(-EBUSY);
		}
		return NOTIFY_OK;
	}

	return NOTIFY_DONE;
}

static int __init migration_init(void)
{
	void *cpu = (void *)(long)smp_processor_id();
	int err;

	/* Initialize migration for the boot CPU */
	err = migration_call(&migration_notifier, CPU_UP_PREPARE, cpu);
	BUG_ON(err == NOTIFY_BAD);
	migration_call(&migration_notifier, CPU_ONLINE, cpu);
	register_cpu_notifier(&migration_notifier);

	/* Register cpu active notifiers */
	cpu_notifier(sched_cpu_active, CPU_PRI_SCHED_ACTIVE);
	cpu_notifier(sched_cpu_inactive, CPU_PRI_SCHED_INACTIVE);

	return 0;
}
early_initcall(migration_init);
#endif

#ifdef CONFIG_SMP

static cpumask_var_t sched_domains_tmpmask; /* sched_domains_mutex */

#ifdef CONFIG_SCHED_DEBUG

static __read_mostly int sched_debug_enabled;

static int __init sched_debug_setup(char *str)
{
	sched_debug_enabled = 1;

	return 0;
}
early_param("sched_debug", sched_debug_setup);

static inline bool sched_debug(void)
{
	return sched_debug_enabled;
}

static int sched_domain_debug_one(struct sched_domain *sd, int cpu, int level,
				  struct cpumask *groupmask)
{
	struct sched_group *group = sd->groups;
	char str[256];

	cpulist_scnprintf(str, sizeof(str), sched_domain_span(sd));
	cpumask_clear(groupmask);

	printk(KERN_DEBUG "%*s domain %d: ", level, "", level);

	if (!(sd->flags & SD_LOAD_BALANCE)) {
		printk("does not load-balance\n");
		if (sd->parent)
			printk(KERN_ERR "ERROR: !SD_LOAD_BALANCE domain"
					" has parent");
		return -1;
	}

	printk(KERN_CONT "span %s level %s\n", str, sd->name);

	if (!cpumask_test_cpu(cpu, sched_domain_span(sd))) {
		printk(KERN_ERR "ERROR: domain->span does not contain "
				"CPU%d\n", cpu);
	}
	if (!cpumask_test_cpu(cpu, sched_group_cpus(group))) {
		printk(KERN_ERR "ERROR: domain->groups does not contain"
				" CPU%d\n", cpu);
	}

	printk(KERN_DEBUG "%*s groups:", level + 1, "");
	do {
		if (!group) {
			printk("\n");
			printk(KERN_ERR "ERROR: group is NULL\n");
			break;
		}

		/*
		 * Even though we initialize ->capacity to something semi-sane,
		 * we leave capacity_orig unset. This allows us to detect if
		 * domain iteration is still funny without causing /0 traps.
		 */
		if (!group->sgc->capacity_orig) {
			printk(KERN_CONT "\n");
			printk(KERN_ERR "ERROR: domain->cpu_capacity not set\n");
			break;
		}

		if (!cpumask_weight(sched_group_cpus(group))) {
			printk(KERN_CONT "\n");
			printk(KERN_ERR "ERROR: empty group\n");
			break;
		}

		if (!(sd->flags & SD_OVERLAP) &&
		    cpumask_intersects(groupmask, sched_group_cpus(group))) {
			printk(KERN_CONT "\n");
			printk(KERN_ERR "ERROR: repeated CPUs\n");
			break;
		}

		cpumask_or(groupmask, groupmask, sched_group_cpus(group));

		cpulist_scnprintf(str, sizeof(str), sched_group_cpus(group));

		printk(KERN_CONT " %s", str);
		if (group->sgc->capacity != SCHED_CAPACITY_SCALE) {
			printk(KERN_CONT " (cpu_capacity = %d)",
				group->sgc->capacity);
		}

		group = group->next;
	} while (group != sd->groups);
	printk(KERN_CONT "\n");

	if (!cpumask_equal(sched_domain_span(sd), groupmask))
		printk(KERN_ERR "ERROR: groups don't span domain->span\n");

	if (sd->parent &&
	    !cpumask_subset(groupmask, sched_domain_span(sd->parent)))
		printk(KERN_ERR "ERROR: parent span is not a superset "
			"of domain->span\n");
	return 0;
}

static void sched_domain_debug(struct sched_domain *sd, int cpu)
{
	int level = 0;

	if (!sched_debug_enabled)
		return;

	if (!sd) {
		printk(KERN_DEBUG "CPU%d attaching NULL sched-domain.\n", cpu);
		return;
	}

	printk(KERN_DEBUG "CPU%d attaching sched-domain:\n", cpu);

	for (;;) {
		if (sched_domain_debug_one(sd, cpu, level, sched_domains_tmpmask))
			break;
		level++;
		sd = sd->parent;
		if (!sd)
			break;
	}
}
#else /* !CONFIG_SCHED_DEBUG */
# define sched_domain_debug(sd, cpu) do { } while (0)
static inline bool sched_debug(void)
{
	return false;
}
#endif /* CONFIG_SCHED_DEBUG */

static int sd_degenerate(struct sched_domain *sd)
{
	if (cpumask_weight(sched_domain_span(sd)) == 1)
		return 1;

	/* Following flags need at least 2 groups */
	if (sd->flags & (SD_LOAD_BALANCE |
			 SD_BALANCE_NEWIDLE |
			 SD_BALANCE_FORK |
			 SD_BALANCE_EXEC |
			 SD_SHARE_CPUCAPACITY |
			 SD_SHARE_PKG_RESOURCES |
			 SD_SHARE_POWERDOMAIN)) {
		if (sd->groups != sd->groups->next)
			return 0;
	}

	/* Following flags don't use groups */
	if (sd->flags & (SD_WAKE_AFFINE))
		return 0;

	return 1;
}

static int
sd_parent_degenerate(struct sched_domain *sd, struct sched_domain *parent)
{
	unsigned long cflags = sd->flags, pflags = parent->flags;

	if (sd_degenerate(parent))
		return 1;

	if (!cpumask_equal(sched_domain_span(sd), sched_domain_span(parent)))
		return 0;

	/* Flags needing groups don't count if only 1 group in parent */
	if (parent->groups == parent->groups->next) {
		pflags &= ~(SD_LOAD_BALANCE |
				SD_BALANCE_NEWIDLE |
				SD_BALANCE_FORK |
				SD_BALANCE_EXEC |
				SD_SHARE_CPUCAPACITY |
				SD_SHARE_PKG_RESOURCES |
				SD_PREFER_SIBLING |
				SD_SHARE_POWERDOMAIN);
		if (nr_node_ids == 1)
			pflags &= ~SD_SERIALIZE;
	}
	if (~cflags & pflags)
		return 0;

	return 1;
}

static void free_rootdomain(struct rcu_head *rcu)
{
	struct root_domain *rd = container_of(rcu, struct root_domain, rcu);

	cpupri_cleanup(&rd->cpupri);
	cpudl_cleanup(&rd->cpudl);
	free_cpumask_var(rd->dlo_mask);
	free_cpumask_var(rd->rto_mask);
	free_cpumask_var(rd->online);
	free_cpumask_var(rd->span);
	kfree(rd);
}

static void rq_attach_root(struct rq *rq, struct root_domain *rd)
{
	struct root_domain *old_rd = NULL;
	unsigned long flags;

	raw_spin_lock_irqsave(&rq->lock, flags);

	if (rq->rd) {
		old_rd = rq->rd;

		if (cpumask_test_cpu(rq->cpu, old_rd->online))
			set_rq_offline(rq);

		cpumask_clear_cpu(rq->cpu, old_rd->span);

		/*
		 * If we dont want to free the old_rd yet then
		 * set old_rd to NULL to skip the freeing later
		 * in this function:
		 */
		if (!atomic_dec_and_test(&old_rd->refcount))
			old_rd = NULL;
	}

	atomic_inc(&rd->refcount);
	rq->rd = rd;

	cpumask_set_cpu(rq->cpu, rd->span);
	if (cpumask_test_cpu(rq->cpu, cpu_active_mask))
		set_rq_online(rq);

	raw_spin_unlock_irqrestore(&rq->lock, flags);

	if (old_rd)
		call_rcu_sched(&old_rd->rcu, free_rootdomain);
}

static int init_rootdomain(struct root_domain *rd)
{
	memset(rd, 0, sizeof(*rd));

	if (!alloc_cpumask_var(&rd->span, GFP_KERNEL))
		goto out;
	if (!alloc_cpumask_var(&rd->online, GFP_KERNEL))
		goto free_span;
	if (!alloc_cpumask_var(&rd->dlo_mask, GFP_KERNEL))
		goto free_online;
	if (!alloc_cpumask_var(&rd->rto_mask, GFP_KERNEL))
		goto free_dlo_mask;

	init_dl_bw(&rd->dl_bw);
	if (cpudl_init(&rd->cpudl) != 0)
		goto free_dlo_mask;

	if (cpupri_init(&rd->cpupri) != 0)
		goto free_rto_mask;
	return 0;

free_rto_mask:
	free_cpumask_var(rd->rto_mask);
free_dlo_mask:
	free_cpumask_var(rd->dlo_mask);
free_online:
	free_cpumask_var(rd->online);
free_span:
	free_cpumask_var(rd->span);
out:
	return -ENOMEM;
}

/*
 * By default the system creates a single root-domain with all cpus as
 * members (mimicking the global state we have today).
 */
struct root_domain def_root_domain;

static void init_defrootdomain(void)
{
	init_rootdomain(&def_root_domain);

	atomic_set(&def_root_domain.refcount, 1);
}

static struct root_domain *alloc_rootdomain(void)
{
	struct root_domain *rd;

	rd = kmalloc(sizeof(*rd), GFP_KERNEL);
	if (!rd)
		return NULL;

	if (init_rootdomain(rd) != 0) {
		kfree(rd);
		return NULL;
	}

	return rd;
}

static void free_sched_groups(struct sched_group *sg, int free_sgc)
{
	struct sched_group *tmp, *first;

	if (!sg)
		return;

	first = sg;
	do {
		tmp = sg->next;

		if (free_sgc && atomic_dec_and_test(&sg->sgc->ref))
			kfree(sg->sgc);

		kfree(sg);
		sg = tmp;
	} while (sg != first);
}

static void free_sched_domain(struct rcu_head *rcu)
{
	struct sched_domain *sd = container_of(rcu, struct sched_domain, rcu);

	/*
	 * If its an overlapping domain it has private groups, iterate and
	 * nuke them all.
	 */
	if (sd->flags & SD_OVERLAP) {
		free_sched_groups(sd->groups, 1);
	} else if (atomic_dec_and_test(&sd->groups->ref)) {
		kfree(sd->groups->sgc);
		kfree(sd->groups);
	}
	kfree(sd);
}

static void destroy_sched_domain(struct sched_domain *sd, int cpu)
{
	call_rcu(&sd->rcu, free_sched_domain);
}

static void destroy_sched_domains(struct sched_domain *sd, int cpu)
{
	for (; sd; sd = sd->parent)
		destroy_sched_domain(sd, cpu);
}

/*
 * Keep a special pointer to the highest sched_domain that has
 * SD_SHARE_PKG_RESOURCE set (Last Level Cache Domain) for this
 * allows us to avoid some pointer chasing select_idle_sibling().
 *
 * Also keep a unique ID per domain (we use the first cpu number in
 * the cpumask of the domain), this allows us to quickly tell if
 * two cpus are in the same cache domain, see cpus_share_cache().
 */
DEFINE_PER_CPU(struct sched_domain *, sd_llc);
DEFINE_PER_CPU(int, sd_llc_size);
DEFINE_PER_CPU(int, sd_llc_id);
DEFINE_PER_CPU(struct sched_domain *, sd_numa);
DEFINE_PER_CPU(struct sched_domain *, sd_busy);
DEFINE_PER_CPU(struct sched_domain *, sd_asym);

static void update_top_cache_domain(int cpu)
{
	struct sched_domain *sd;
	struct sched_domain *busy_sd = NULL;
	int id = cpu;
	int size = 1;

	sd = highest_flag_domain(cpu, SD_SHARE_PKG_RESOURCES);
	if (sd) {
		id = cpumask_first(sched_domain_span(sd));
		size = cpumask_weight(sched_domain_span(sd));
		busy_sd = sd->parent; /* sd_busy */
	}
	rcu_assign_pointer(per_cpu(sd_busy, cpu), busy_sd);

	rcu_assign_pointer(per_cpu(sd_llc, cpu), sd);
	per_cpu(sd_llc_size, cpu) = size;
	per_cpu(sd_llc_id, cpu) = id;

	sd = lowest_flag_domain(cpu, SD_NUMA);
	rcu_assign_pointer(per_cpu(sd_numa, cpu), sd);

	sd = highest_flag_domain(cpu, SD_ASYM_PACKING);
	rcu_assign_pointer(per_cpu(sd_asym, cpu), sd);
}

/*
 * Attach the domain 'sd' to 'cpu' as its base domain. Callers must
 * hold the hotplug lock.
 */
static void
cpu_attach_domain(struct sched_domain *sd, struct root_domain *rd, int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	struct sched_domain *tmp;
	unsigned long next_balance = rq->next_balance;

	/* Remove the sched domains which do not contribute to scheduling. */
	for (tmp = sd; tmp; ) {
		struct sched_domain *parent = tmp->parent;
		if (!parent)
			break;

		if (sd_parent_degenerate(tmp, parent)) {
			tmp->parent = parent->parent;
			if (parent->parent)
				parent->parent->child = tmp;
			/*
			 * Transfer SD_PREFER_SIBLING down in case of a
			 * degenerate parent; the spans match for this
			 * so the property transfers.
			 */
			if (parent->flags & SD_PREFER_SIBLING)
				tmp->flags |= SD_PREFER_SIBLING;
			destroy_sched_domain(parent, cpu);
		} else
			tmp = tmp->parent;
	}

	if (sd && sd_degenerate(sd)) {
		tmp = sd;
		sd = sd->parent;
		destroy_sched_domain(tmp, cpu);
		if (sd)
			sd->child = NULL;
	}

	for (tmp = sd; tmp; ) {
		unsigned long interval;

		interval = msecs_to_jiffies(tmp->balance_interval);
		if (time_after(next_balance, tmp->last_balance + interval))
			next_balance = tmp->last_balance + interval;

		tmp = tmp->parent;
	}
	rq->next_balance = next_balance;

	sched_domain_debug(sd, cpu);

	rq_attach_root(rq, rd);
	tmp = rq->sd;
	rcu_assign_pointer(rq->sd, sd);
	destroy_sched_domains(tmp, cpu);

	update_top_cache_domain(cpu);
}

/* cpus with isolated domains */
static cpumask_var_t cpu_isolated_map;

/* Setup the mask of cpus configured for isolated domains */
static int __init isolated_cpu_setup(char *str)
{
	alloc_bootmem_cpumask_var(&cpu_isolated_map);
	cpulist_parse(str, cpu_isolated_map);
	return 1;
}

__setup("isolcpus=", isolated_cpu_setup);

struct s_data {
	struct sched_domain ** __percpu sd;
	struct root_domain	*rd;
};

enum s_alloc {
	sa_rootdomain,
	sa_sd,
	sa_sd_storage,
	sa_none,
};

/*
 * Build an iteration mask that can exclude certain CPUs from the upwards
 * domain traversal.
 *
 * Asymmetric node setups can result in situations where the domain tree is of
 * unequal depth, make sure to skip domains that already cover the entire
 * range.
 *
 * In that case build_sched_domains() will have terminated the iteration early
 * and our sibling sd spans will be empty. Domains should always include the
 * cpu they're built on, so check that.
 *
 */
static void build_group_mask(struct sched_domain *sd, struct sched_group *sg)
{
	const struct cpumask *span = sched_domain_span(sd);
	struct sd_data *sdd = sd->private;
	struct sched_domain *sibling;
	int i;

	for_each_cpu(i, span) {
		sibling = *per_cpu_ptr(sdd->sd, i);
		if (!cpumask_test_cpu(i, sched_domain_span(sibling)))
			continue;

		cpumask_set_cpu(i, sched_group_mask(sg));
	}
}

/*
 * Return the canonical balance cpu for this group, this is the first cpu
 * of this group that's also in the iteration mask.
 */
int group_balance_cpu(struct sched_group *sg)
{
	return cpumask_first_and(sched_group_cpus(sg), sched_group_mask(sg));
}

static int
build_overlap_sched_groups(struct sched_domain *sd, int cpu)
{
	struct sched_group *first = NULL, *last = NULL, *groups = NULL, *sg;
	const struct cpumask *span = sched_domain_span(sd);
	struct cpumask *covered = sched_domains_tmpmask;
	struct sd_data *sdd = sd->private;
	struct sched_domain *sibling;
	int i;

	cpumask_clear(covered);

	for_each_cpu(i, span) {
		struct cpumask *sg_span;

		if (cpumask_test_cpu(i, covered))
			continue;

		sibling = *per_cpu_ptr(sdd->sd, i);

		/* See the comment near build_group_mask(). */
		if (!cpumask_test_cpu(i, sched_domain_span(sibling)))
			continue;

		sg = kzalloc_node(sizeof(struct sched_group) + cpumask_size(),
				GFP_KERNEL, cpu_to_node(cpu));

		if (!sg)
			goto fail;

		sg_span = sched_group_cpus(sg);
		if (sibling->child)
			cpumask_copy(sg_span, sched_domain_span(sibling->child));
		else
			cpumask_set_cpu(i, sg_span);

		cpumask_or(covered, covered, sg_span);

		sg->sgc = *per_cpu_ptr(sdd->sgc, i);
		if (atomic_inc_return(&sg->sgc->ref) == 1)
			build_group_mask(sd, sg);

		/*
		 * Initialize sgc->capacity such that even if we mess up the
		 * domains and no possible iteration will get us here, we won't
		 * die on a /0 trap.
		 */
		sg->sgc->capacity = SCHED_CAPACITY_SCALE * cpumask_weight(sg_span);
		sg->sgc->capacity_orig = sg->sgc->capacity;

		/*
		 * Make sure the first group of this domain contains the
		 * canonical balance cpu. Otherwise the sched_domain iteration
		 * breaks. See update_sg_lb_stats().
		 */
		if ((!groups && cpumask_test_cpu(cpu, sg_span)) ||
		    group_balance_cpu(sg) == cpu)
			groups = sg;

		if (!first)
			first = sg;
		if (last)
			last->next = sg;
		last = sg;
		last->next = first;
	}
	sd->groups = groups;

	return 0;

fail:
	free_sched_groups(first, 0);

	return -ENOMEM;
}

static int get_group(int cpu, struct sd_data *sdd, struct sched_group **sg)
{
	struct sched_domain *sd = *per_cpu_ptr(sdd->sd, cpu);
	struct sched_domain *child = sd->child;

	if (child)
		cpu = cpumask_first(sched_domain_span(child));

	if (sg) {
		*sg = *per_cpu_ptr(sdd->sg, cpu);
		(*sg)->sgc = *per_cpu_ptr(sdd->sgc, cpu);
		atomic_set(&(*sg)->sgc->ref, 1); /* for claim_allocations */
	}

	return cpu;
}

/*
 * build_sched_groups will build a circular linked list of the groups
 * covered by the given span, and will set each group's ->cpumask correctly,
 * and ->cpu_capacity to 0.
 *
 * Assumes the sched_domain tree is fully constructed
 */
static int
build_sched_groups(struct sched_domain *sd, int cpu)
{
	struct sched_group *first = NULL, *last = NULL;
	struct sd_data *sdd = sd->private;
	const struct cpumask *span = sched_domain_span(sd);
	struct cpumask *covered;
	int i;

	get_group(cpu, sdd, &sd->groups);
	atomic_inc(&sd->groups->ref);

	if (cpu != cpumask_first(span))
		return 0;

	lockdep_assert_held(&sched_domains_mutex);
	covered = sched_domains_tmpmask;

	cpumask_clear(covered);

	for_each_cpu(i, span) {
		struct sched_group *sg;
		int group, j;

		if (cpumask_test_cpu(i, covered))
			continue;

		group = get_group(i, sdd, &sg);
		cpumask_setall(sched_group_mask(sg));

		for_each_cpu(j, span) {
			if (get_group(j, sdd, NULL) != group)
				continue;

			cpumask_set_cpu(j, covered);
			cpumask_set_cpu(j, sched_group_cpus(sg));
		}

		if (!first)
			first = sg;
		if (last)
			last->next = sg;
		last = sg;
	}
	last->next = first;

	return 0;
}

/*
 * Initialize sched groups cpu_capacity.
 *
 * cpu_capacity indicates the capacity of sched group, which is used while
 * distributing the load between different sched groups in a sched domain.
 * Typically cpu_capacity for all the groups in a sched domain will be same
 * unless there are asymmetries in the topology. If there are asymmetries,
 * group having more cpu_capacity will pickup more load compared to the
 * group having less cpu_capacity.
 */
static void init_sched_groups_capacity(int cpu, struct sched_domain *sd)
{
	struct sched_group *sg = sd->groups;

	WARN_ON(!sg);

	do {
		sg->group_weight = cpumask_weight(sched_group_cpus(sg));
		sg = sg->next;
	} while (sg != sd->groups);

	if (cpu != group_balance_cpu(sg))
		return;

	update_group_capacity(sd, cpu);
	atomic_set(&sg->sgc->nr_busy_cpus, sg->group_weight);
}

/*
 * Initializers for schedule domains
 * Non-inlined to reduce accumulated stack pressure in build_sched_domains()
 */

static int default_relax_domain_level = -1;
int sched_domain_level_max;

static int __init setup_relax_domain_level(char *str)
{
	if (kstrtoint(str, 0, &default_relax_domain_level))
		pr_warn("Unable to set relax_domain_level\n");

	return 1;
}
__setup("relax_domain_level=", setup_relax_domain_level);

static void set_domain_attribute(struct sched_domain *sd,
				 struct sched_domain_attr *attr)
{
	int request;

	if (!attr || attr->relax_domain_level < 0) {
		if (default_relax_domain_level < 0)
			return;
		else
			request = default_relax_domain_level;
	} else
		request = attr->relax_domain_level;
	if (request < sd->level) {
		/* turn off idle balance on this domain */
		sd->flags &= ~(SD_BALANCE_WAKE|SD_BALANCE_NEWIDLE);
	} else {
		/* turn on idle balance on this domain */
		sd->flags |= (SD_BALANCE_WAKE|SD_BALANCE_NEWIDLE);
	}
}

static void __sdt_free(const struct cpumask *cpu_map);
static int __sdt_alloc(const struct cpumask *cpu_map);

static void __free_domain_allocs(struct s_data *d, enum s_alloc what,
				 const struct cpumask *cpu_map)
{
	switch (what) {
	case sa_rootdomain:
		if (!atomic_read(&d->rd->refcount))
			free_rootdomain(&d->rd->rcu); /* fall through */
	case sa_sd:
		free_percpu(d->sd); /* fall through */
	case sa_sd_storage:
		__sdt_free(cpu_map); /* fall through */
	case sa_none:
		break;
	}
}

static enum s_alloc __visit_domain_allocation_hell(struct s_data *d,
						   const struct cpumask *cpu_map)
{
	memset(d, 0, sizeof(*d));

	if (__sdt_alloc(cpu_map))
		return sa_sd_storage;
	d->sd = alloc_percpu(struct sched_domain *);
	if (!d->sd)
		return sa_sd_storage;
	d->rd = alloc_rootdomain();
	if (!d->rd)
		return sa_sd;
	return sa_rootdomain;
}

/*
 * NULL the sd_data elements we've used to build the sched_domain and
 * sched_group structure so that the subsequent __free_domain_allocs()
 * will not free the data we're using.
 */
static void claim_allocations(int cpu, struct sched_domain *sd)
{
	struct sd_data *sdd = sd->private;

	WARN_ON_ONCE(*per_cpu_ptr(sdd->sd, cpu) != sd);
	*per_cpu_ptr(sdd->sd, cpu) = NULL;

	if (atomic_read(&(*per_cpu_ptr(sdd->sg, cpu))->ref))
		*per_cpu_ptr(sdd->sg, cpu) = NULL;

	if (atomic_read(&(*per_cpu_ptr(sdd->sgc, cpu))->ref))
		*per_cpu_ptr(sdd->sgc, cpu) = NULL;
}

#ifdef CONFIG_NUMA
static int sched_domains_numa_levels;
static int *sched_domains_numa_distance;
static struct cpumask ***sched_domains_numa_masks;
static int sched_domains_curr_level;
#endif

/*
 * SD_flags allowed in topology descriptions.
 *
 * SD_SHARE_CPUCAPACITY      - describes SMT topologies
 * SD_SHARE_PKG_RESOURCES - describes shared caches
 * SD_NUMA                - describes NUMA topologies
 * SD_SHARE_POWERDOMAIN   - describes shared power domain
 *
 * Odd one out:
 * SD_ASYM_PACKING        - describes SMT quirks
 */
#define TOPOLOGY_SD_FLAGS		\
	(SD_SHARE_CPUCAPACITY |		\
	 SD_SHARE_PKG_RESOURCES |	\
	 SD_NUMA |			\
	 SD_ASYM_PACKING |		\
	 SD_SHARE_POWERDOMAIN)

static struct sched_domain *
sd_init(struct sched_domain_topology_level *tl, int cpu)
{
	struct sched_domain *sd = *per_cpu_ptr(tl->data.sd, cpu);
	int sd_weight, sd_flags = 0;

#ifdef CONFIG_NUMA
	/*
	 * Ugly hack to pass state to sd_numa_mask()...
	 */
	sched_domains_curr_level = tl->numa_level;
#endif

	sd_weight = cpumask_weight(tl->mask(cpu));

	if (tl->sd_flags)
		sd_flags = (*tl->sd_flags)();
	if (WARN_ONCE(sd_flags & ~TOPOLOGY_SD_FLAGS,
			"wrong sd_flags in topology description\n"))
		sd_flags &= ~TOPOLOGY_SD_FLAGS;

	*sd = (struct sched_domain){
		.min_interval		= sd_weight,
		.max_interval		= 2*sd_weight,
		.busy_factor		= 32,
		.imbalance_pct		= 125,

		.cache_nice_tries	= 0,
		.busy_idx		= 0,
		.idle_idx		= 0,
		.newidle_idx		= 0,
		.wake_idx		= 0,
		.forkexec_idx		= 0,

		.flags			= 1*SD_LOAD_BALANCE
					| 1*SD_BALANCE_NEWIDLE
					| 1*SD_BALANCE_EXEC
					| 1*SD_BALANCE_FORK
					| 0*SD_BALANCE_WAKE
					| 1*SD_WAKE_AFFINE
					| 0*SD_SHARE_CPUCAPACITY
					| 0*SD_SHARE_PKG_RESOURCES
					| 0*SD_SERIALIZE
					| 0*SD_PREFER_SIBLING
					| 0*SD_NUMA
					| sd_flags
					,

		.last_balance		= jiffies,
		.balance_interval	= sd_weight,
		.smt_gain		= 0,
		.max_newidle_lb_cost	= 0,
		.next_decay_max_lb_cost	= jiffies,
#ifdef CONFIG_SCHED_DEBUG
		.name			= tl->name,
#endif
	};

	/*
	 * Convert topological properties into behaviour.
	 */

	if (sd->flags & SD_SHARE_CPUCAPACITY) {
		sd->imbalance_pct = 110;
		sd->smt_gain = 1178; /* ~15% */

	} else if (sd->flags & SD_SHARE_PKG_RESOURCES) {
		sd->imbalance_pct = 117;
		sd->cache_nice_tries = 1;
		sd->busy_idx = 2;

#ifdef CONFIG_NUMA
	} else if (sd->flags & SD_NUMA) {
		sd->cache_nice_tries = 2;
		sd->busy_idx = 3;
		sd->idle_idx = 2;

		sd->flags |= SD_SERIALIZE;
		if (sched_domains_numa_distance[tl->numa_level] > RECLAIM_DISTANCE) {
			sd->flags &= ~(SD_BALANCE_EXEC |
				       SD_BALANCE_FORK |
				       SD_WAKE_AFFINE);
		}

#endif
	} else {
		sd->flags |= SD_PREFER_SIBLING;
		sd->cache_nice_tries = 1;
		sd->busy_idx = 2;
		sd->idle_idx = 1;
	}

	sd->private = &tl->data;

	return sd;
}

/*
 * Topology list, bottom-up.
 */
static struct sched_domain_topology_level default_topology[] = {
#ifdef CONFIG_SCHED_SMT
	{ cpu_smt_mask, cpu_smt_flags, SD_INIT_NAME(SMT) },
#endif
#ifdef CONFIG_SCHED_MC
	{ cpu_coregroup_mask, cpu_core_flags, SD_INIT_NAME(MC) },
#endif
	{ cpu_cpu_mask, SD_INIT_NAME(DIE) },
	{ NULL, },
};

struct sched_domain_topology_level *sched_domain_topology = default_topology;

#define for_each_sd_topology(tl)			\
	for (tl = sched_domain_topology; tl->mask; tl++)

void set_sched_topology(struct sched_domain_topology_level *tl)
{
	sched_domain_topology = tl;
}

#ifdef CONFIG_NUMA

static const struct cpumask *sd_numa_mask(int cpu)
{
	return sched_domains_numa_masks[sched_domains_curr_level][cpu_to_node(cpu)];
}

static void sched_numa_warn(const char *str)
{
	static int done = false;
	int i,j;

	if (done)
		return;

	done = true;

	printk(KERN_WARNING "ERROR: %s\n\n", str);

	for (i = 0; i < nr_node_ids; i++) {
		printk(KERN_WARNING "  ");
		for (j = 0; j < nr_node_ids; j++)
			printk(KERN_CONT "%02d ", node_distance(i,j));
		printk(KERN_CONT "\n");
	}
	printk(KERN_WARNING "\n");
}

static bool find_numa_distance(int distance)
{
	int i;

	if (distance == node_distance(0, 0))
		return true;

	for (i = 0; i < sched_domains_numa_levels; i++) {
		if (sched_domains_numa_distance[i] == distance)
			return true;
	}

	return false;
}

static void sched_init_numa(void)
{
	int next_distance, curr_distance = node_distance(0, 0);
	struct sched_domain_topology_level *tl;
	int level = 0;
	int i, j, k;

	sched_domains_numa_distance = kzalloc(sizeof(int) * nr_node_ids, GFP_KERNEL);
	if (!sched_domains_numa_distance)
		return;

	/*
	 * O(nr_nodes^2) deduplicating selection sort -- in order to find the
	 * unique distances in the node_distance() table.
	 *
	 * Assumes node_distance(0,j) includes all distances in
	 * node_distance(i,j) in order to avoid cubic time.
	 */
	next_distance = curr_distance;
	for (i = 0; i < nr_node_ids; i++) {
		for (j = 0; j < nr_node_ids; j++) {
			for (k = 0; k < nr_node_ids; k++) {
				int distance = node_distance(i, k);

				if (distance > curr_distance &&
				    (distance < next_distance ||
				     next_distance == curr_distance))
					next_distance = distance;

				/*
				 * While not a strong assumption it would be nice to know
				 * about cases where if node A is connected to B, B is not
				 * equally connected to A.
				 */
				if (sched_debug() && node_distance(k, i) != distance)
					sched_numa_warn("Node-distance not symmetric");

				if (sched_debug() && i && !find_numa_distance(distance))
					sched_numa_warn("Node-0 not representative");
			}
			if (next_distance != curr_distance) {
				sched_domains_numa_distance[level++] = next_distance;
				sched_domains_numa_levels = level;
				curr_distance = next_distance;
			} else break;
		}

		/*
		 * In case of sched_debug() we verify the above assumption.
		 */
		if (!sched_debug())
			break;
	}

	if (!level)
		return;

	/*
	 * 'level' contains the number of unique distances, excluding the
	 * identity distance node_distance(i,i).
	 *
	 * The sched_domains_numa_distance[] array includes the actual distance
	 * numbers.
	 */

	/*
	 * Here, we should temporarily reset sched_domains_numa_levels to 0.
	 * If it fails to allocate memory for array sched_domains_numa_masks[][],
	 * the array will contain less then 'level' members. This could be
	 * dangerous when we use it to iterate array sched_domains_numa_masks[][]
	 * in other functions.
	 *
	 * We reset it to 'level' at the end of this function.
	 */
	sched_domains_numa_levels = 0;

	sched_domains_numa_masks = kzalloc(sizeof(void *) * level, GFP_KERNEL);
	if (!sched_domains_numa_masks)
		return;

	/*
	 * Now for each level, construct a mask per node which contains all
	 * cpus of nodes that are that many hops away from us.
	 */
	for (i = 0; i < level; i++) {
		sched_domains_numa_masks[i] =
			kzalloc(nr_node_ids * sizeof(void *), GFP_KERNEL);
		if (!sched_domains_numa_masks[i])
			return;

		for (j = 0; j < nr_node_ids; j++) {
			struct cpumask *mask = kzalloc(cpumask_size(), GFP_KERNEL);
			if (!mask)
				return;

			sched_domains_numa_masks[i][j] = mask;

			for_each_node(k) {
				if (node_distance(j, k) > sched_domains_numa_distance[i])
					continue;

				cpumask_or(mask, mask, cpumask_of_node(k));
			}
		}
	}

	/* Compute default topology size */
	for (i = 0; sched_domain_topology[i].mask; i++);

	tl = kzalloc((i + level + 1) *
			sizeof(struct sched_domain_topology_level), GFP_KERNEL);
	if (!tl)
		return;

	/*
	 * Copy the default topology bits..
	 */
	for (i = 0; sched_domain_topology[i].mask; i++)
		tl[i] = sched_domain_topology[i];

	/*
	 * .. and append 'j' levels of NUMA goodness.
	 */
	for (j = 0; j < level; i++, j++) {
		tl[i] = (struct sched_domain_topology_level){
			.mask = sd_numa_mask,
			.sd_flags = cpu_numa_flags,
			.flags = SDTL_OVERLAP,
			.numa_level = j,
			SD_INIT_NAME(NUMA)
		};
	}

	sched_domain_topology = tl;

	sched_domains_numa_levels = level;
}

static void sched_domains_numa_masks_set(int cpu)
{
	int i, j;
	int node = cpu_to_node(cpu);

	for (i = 0; i < sched_domains_numa_levels; i++) {
		for (j = 0; j < nr_node_ids; j++) {
			if (node_distance(j, node) <= sched_domains_numa_distance[i])
				cpumask_set_cpu(cpu, sched_domains_numa_masks[i][j]);
		}
	}
}

static void sched_domains_numa_masks_clear(int cpu)
{
	int i, j;
	for (i = 0; i < sched_domains_numa_levels; i++) {
		for (j = 0; j < nr_node_ids; j++)
			cpumask_clear_cpu(cpu, sched_domains_numa_masks[i][j]);
	}
}

/*
 * Update sched_domains_numa_masks[level][node] array when new cpus
 * are onlined.
 */
static int sched_domains_numa_masks_update(struct notifier_block *nfb,
					   unsigned long action,
					   void *hcpu)
{
	int cpu = (long)hcpu;

	switch (action & ~CPU_TASKS_FROZEN) {
	case CPU_ONLINE:
		sched_domains_numa_masks_set(cpu);
		break;

	case CPU_DEAD:
		sched_domains_numa_masks_clear(cpu);
		break;

	default:
		return NOTIFY_DONE;
	}

	return NOTIFY_OK;
}
#else
static inline void sched_init_numa(void)
{
}

static int sched_domains_numa_masks_update(struct notifier_block *nfb,
					   unsigned long action,
					   void *hcpu)
{
	return 0;
}
#endif /* CONFIG_NUMA */

static int __sdt_alloc(const struct cpumask *cpu_map)
{
	struct sched_domain_topology_level *tl;
	int j;

	for_each_sd_topology(tl) {
		struct sd_data *sdd = &tl->data;

		sdd->sd = alloc_percpu(struct sched_domain *);
		if (!sdd->sd)
			return -ENOMEM;

		sdd->sg = alloc_percpu(struct sched_group *);
		if (!sdd->sg)
			return -ENOMEM;

		sdd->sgc = alloc_percpu(struct sched_group_capacity *);
		if (!sdd->sgc)
			return -ENOMEM;

		for_each_cpu(j, cpu_map) {
			struct sched_domain *sd;
			struct sched_group *sg;
			struct sched_group_capacity *sgc;

		       	sd = kzalloc_node(sizeof(struct sched_domain) + cpumask_size(),
					GFP_KERNEL, cpu_to_node(j));
			if (!sd)
				return -ENOMEM;

			*per_cpu_ptr(sdd->sd, j) = sd;

			sg = kzalloc_node(sizeof(struct sched_group) + cpumask_size(),
					GFP_KERNEL, cpu_to_node(j));
			if (!sg)
				return -ENOMEM;

			sg->next = sg;

			*per_cpu_ptr(sdd->sg, j) = sg;

			sgc = kzalloc_node(sizeof(struct sched_group_capacity) + cpumask_size(),
					GFP_KERNEL, cpu_to_node(j));
			if (!sgc)
				return -ENOMEM;

			*per_cpu_ptr(sdd->sgc, j) = sgc;
		}
	}

	return 0;
}

static void __sdt_free(const struct cpumask *cpu_map)
{
	struct sched_domain_topology_level *tl;
	int j;

	for_each_sd_topology(tl) {
		struct sd_data *sdd = &tl->data;

		for_each_cpu(j, cpu_map) {
			struct sched_domain *sd;

			if (sdd->sd) {
				sd = *per_cpu_ptr(sdd->sd, j);
				if (sd && (sd->flags & SD_OVERLAP))
					free_sched_groups(sd->groups, 0);
				kfree(*per_cpu_ptr(sdd->sd, j));
			}

			if (sdd->sg)
				kfree(*per_cpu_ptr(sdd->sg, j));
			if (sdd->sgc)
				kfree(*per_cpu_ptr(sdd->sgc, j));
		}
		free_percpu(sdd->sd);
		sdd->sd = NULL;
		free_percpu(sdd->sg);
		sdd->sg = NULL;
		free_percpu(sdd->sgc);
		sdd->sgc = NULL;
	}
}

struct sched_domain *build_sched_domain(struct sched_domain_topology_level *tl,
		const struct cpumask *cpu_map, struct sched_domain_attr *attr,
		struct sched_domain *child, int cpu)
{
	struct sched_domain *sd = sd_init(tl, cpu);
	if (!sd)
		return child;

	cpumask_and(sched_domain_span(sd), cpu_map, tl->mask(cpu));
	if (child) {
		sd->level = child->level + 1;
		sched_domain_level_max = max(sched_domain_level_max, sd->level);
		child->parent = sd;
		sd->child = child;

		if (!cpumask_subset(sched_domain_span(child),
				    sched_domain_span(sd))) {
			pr_err("BUG: arch topology borken\n");
#ifdef CONFIG_SCHED_DEBUG
			pr_err("     the %s domain not a subset of the %s domain\n",
					child->name, sd->name);
#endif
#ifdef CONFIG_PANIC_ON_SCHED_BUG
			BUG();
#endif
			/* Fixup, ensure @sd has at least @child cpus. */
			cpumask_or(sched_domain_span(sd),
				   sched_domain_span(sd),
				   sched_domain_span(child));
		}

	}
	set_domain_attribute(sd, attr);

	return sd;
}

/*
 * Build sched domains for a given set of cpus and attach the sched domains
 * to the individual cpus
 */
static int build_sched_domains(const struct cpumask *cpu_map,
			       struct sched_domain_attr *attr)
{
	enum s_alloc alloc_state;
	struct sched_domain *sd;
	struct s_data d;
	int i, ret = -ENOMEM;

	alloc_state = __visit_domain_allocation_hell(&d, cpu_map);
	if (alloc_state != sa_rootdomain)
		goto error;

	/* Set up domains for cpus specified by the cpu_map. */
	for_each_cpu(i, cpu_map) {
		struct sched_domain_topology_level *tl;

		sd = NULL;
		for_each_sd_topology(tl) {
			sd = build_sched_domain(tl, cpu_map, attr, sd, i);
			if (tl == sched_domain_topology)
				*per_cpu_ptr(d.sd, i) = sd;
			if (tl->flags & SDTL_OVERLAP || sched_feat(FORCE_SD_OVERLAP))
				sd->flags |= SD_OVERLAP;
			if (cpumask_equal(cpu_map, sched_domain_span(sd)))
				break;
		}
	}

	/* Build the groups for the domains */
	for_each_cpu(i, cpu_map) {
		for (sd = *per_cpu_ptr(d.sd, i); sd; sd = sd->parent) {
			sd->span_weight = cpumask_weight(sched_domain_span(sd));
			if (sd->flags & SD_OVERLAP) {
				if (build_overlap_sched_groups(sd, i))
					goto error;
			} else {
				if (build_sched_groups(sd, i))
					goto error;
			}
		}
	}

	/* Calculate CPU capacity for physical packages and nodes */
	for (i = nr_cpumask_bits-1; i >= 0; i--) {
		if (!cpumask_test_cpu(i, cpu_map))
			continue;

		for (sd = *per_cpu_ptr(d.sd, i); sd; sd = sd->parent) {
			claim_allocations(i, sd);
			init_sched_groups_capacity(i, sd);
		}
	}

	/* Attach the domains */
	rcu_read_lock();
	for_each_cpu(i, cpu_map) {
		sd = *per_cpu_ptr(d.sd, i);
		cpu_attach_domain(sd, d.rd, i);
	}
	rcu_read_unlock();

	ret = 0;
error:
	__free_domain_allocs(&d, alloc_state, cpu_map);
	return ret;
}

static cpumask_var_t *doms_cur;	/* current sched domains */
static int ndoms_cur;		/* number of sched domains in 'doms_cur' */
static struct sched_domain_attr *dattr_cur;
				/* attribues of custom domains in 'doms_cur' */

/*
 * Special case: If a kmalloc of a doms_cur partition (array of
 * cpumask) fails, then fallback to a single sched domain,
 * as determined by the single cpumask fallback_doms.
 */
static cpumask_var_t fallback_doms;

/*
 * arch_update_cpu_topology lets virtualized architectures update the
 * cpu core maps. It is supposed to return 1 if the topology changed
 * or 0 if it stayed the same.
 */
int __weak arch_update_cpu_topology(void)
{
	return 0;
}

cpumask_var_t *alloc_sched_domains(unsigned int ndoms)
{
	int i;
	cpumask_var_t *doms;

	doms = kmalloc(sizeof(*doms) * ndoms, GFP_KERNEL);
	if (!doms)
		return NULL;
	for (i = 0; i < ndoms; i++) {
		if (!alloc_cpumask_var(&doms[i], GFP_KERNEL)) {
			free_sched_domains(doms, i);
			return NULL;
		}
	}
	return doms;
}

void free_sched_domains(cpumask_var_t doms[], unsigned int ndoms)
{
	unsigned int i;
	for (i = 0; i < ndoms; i++)
		free_cpumask_var(doms[i]);
	kfree(doms);
}

/*
 * Set up scheduler domains and groups. Callers must hold the hotplug lock.
 * For now this just excludes isolated cpus, but could be used to
 * exclude other special cases in the future.
 */
static int init_sched_domains(const struct cpumask *cpu_map)
{
	int err;

	arch_update_cpu_topology();
	ndoms_cur = 1;
	doms_cur = alloc_sched_domains(ndoms_cur);
	if (!doms_cur)
		doms_cur = &fallback_doms;
	cpumask_andnot(doms_cur[0], cpu_map, cpu_isolated_map);
	err = build_sched_domains(doms_cur[0], NULL);
	register_sched_domain_sysctl();

	return err;
}

/*
 * Detach sched domains from a group of cpus specified in cpu_map
 * These cpus will now be attached to the NULL domain
 */
static void detach_destroy_domains(const struct cpumask *cpu_map)
{
	int i;

	rcu_read_lock();
	for_each_cpu(i, cpu_map)
		cpu_attach_domain(NULL, &def_root_domain, i);
	rcu_read_unlock();
}

/* handle null as "default" */
static int dattrs_equal(struct sched_domain_attr *cur, int idx_cur,
			struct sched_domain_attr *new, int idx_new)
{
	struct sched_domain_attr tmp;

	/* fast path */
	if (!new && !cur)
		return 1;

	tmp = SD_ATTR_INIT;
	return !memcmp(cur ? (cur + idx_cur) : &tmp,
			new ? (new + idx_new) : &tmp,
			sizeof(struct sched_domain_attr));
}

/*
 * Partition sched domains as specified by the 'ndoms_new'
 * cpumasks in the array doms_new[] of cpumasks. This compares
 * doms_new[] to the current sched domain partitioning, doms_cur[].
 * It destroys each deleted domain and builds each new domain.
 *
 * 'doms_new' is an array of cpumask_var_t's of length 'ndoms_new'.
 * The masks don't intersect (don't overlap.) We should setup one
 * sched domain for each mask. CPUs not in any of the cpumasks will
 * not be load balanced. If the same cpumask appears both in the
 * current 'doms_cur' domains and in the new 'doms_new', we can leave
 * it as it is.
 *
 * The passed in 'doms_new' should be allocated using
 * alloc_sched_domains.  This routine takes ownership of it and will
 * free_sched_domains it when done with it. If the caller failed the
 * alloc call, then it can pass in doms_new == NULL && ndoms_new == 1,
 * and partition_sched_domains() will fallback to the single partition
 * 'fallback_doms', it also forces the domains to be rebuilt.
 *
 * If doms_new == NULL it will be replaced with cpu_online_mask.
 * ndoms_new == 0 is a special case for destroying existing domains,
 * and it will not create the default domain.
 *
 * Call with hotplug lock held
 */
void partition_sched_domains(int ndoms_new, cpumask_var_t doms_new[],
			     struct sched_domain_attr *dattr_new)
{
	int i, j, n;
	int new_topology;

	mutex_lock(&sched_domains_mutex);

	/* always unregister in case we don't destroy any domains */
	unregister_sched_domain_sysctl();

	/* Let architecture update cpu core mappings. */
	new_topology = arch_update_cpu_topology();

	n = doms_new ? ndoms_new : 0;

	/* Destroy deleted domains */
	for (i = 0; i < ndoms_cur; i++) {
		for (j = 0; j < n && !new_topology; j++) {
			if (cpumask_equal(doms_cur[i], doms_new[j])
			    && dattrs_equal(dattr_cur, i, dattr_new, j))
				goto match1;
		}
		/* no match - a current sched domain not in new doms_new[] */
		detach_destroy_domains(doms_cur[i]);
match1:
		;
	}

	n = ndoms_cur;
	if (doms_new == NULL) {
		n = 0;
		doms_new = &fallback_doms;
		cpumask_andnot(doms_new[0], cpu_active_mask, cpu_isolated_map);
		WARN_ON_ONCE(dattr_new);
	}

	/* Build new domains */
	for (i = 0; i < ndoms_new; i++) {
		for (j = 0; j < n && !new_topology; j++) {
			if (cpumask_equal(doms_new[i], doms_cur[j])
			    && dattrs_equal(dattr_new, i, dattr_cur, j))
				goto match2;
		}
		/* no match - add a new doms_new */
		build_sched_domains(doms_new[i], dattr_new ? dattr_new + i : NULL);
match2:
		;
	}

	/* Remember the new sched domains */
	if (doms_cur != &fallback_doms)
		free_sched_domains(doms_cur, ndoms_cur);
	kfree(dattr_cur);	/* kfree(NULL) is safe */
	doms_cur = doms_new;
	dattr_cur = dattr_new;
	ndoms_cur = ndoms_new;

	register_sched_domain_sysctl();

	mutex_unlock(&sched_domains_mutex);
}

static int num_cpus_frozen;	/* used to mark begin/end of suspend/resume */

/*
 * Update cpusets according to cpu_active mask.  If cpusets are
 * disabled, cpuset_update_active_cpus() becomes a simple wrapper
 * around partition_sched_domains().
 *
 * If we come here as part of a suspend/resume, don't touch cpusets because we
 * want to restore it back to its original state upon resume anyway.
 */
static int cpuset_cpu_active(struct notifier_block *nfb, unsigned long action,
			     void *hcpu)
{
	switch (action) {
	case CPU_ONLINE_FROZEN:
	case CPU_DOWN_FAILED_FROZEN:

		/*
		 * num_cpus_frozen tracks how many CPUs are involved in suspend
		 * resume sequence. As long as this is not the last online
		 * operation in the resume sequence, just build a single sched
		 * domain, ignoring cpusets.
		 */
		num_cpus_frozen--;
		if (likely(num_cpus_frozen)) {
			partition_sched_domains(1, NULL, NULL);
			break;
		}

		/*
		 * This is the last CPU online operation. So fall through and
		 * restore the original sched domains by considering the
		 * cpuset configurations.
		 */

	case CPU_ONLINE:
	case CPU_DOWN_FAILED:
		cpuset_update_active_cpus(true);
		break;
	default:
		return NOTIFY_DONE;
	}
	return NOTIFY_OK;
}

static int cpuset_cpu_inactive(struct notifier_block *nfb, unsigned long action,
			       void *hcpu)
{
	switch (action) {
	case CPU_DOWN_PREPARE:
		cpuset_update_active_cpus(false);
		break;
	case CPU_DOWN_PREPARE_FROZEN:
		num_cpus_frozen++;
		partition_sched_domains(1, NULL, NULL);
		break;
	default:
		return NOTIFY_DONE;
	}
	return NOTIFY_OK;
}

void __init sched_init_smp(void)
{
	cpumask_var_t non_isolated_cpus;

	alloc_cpumask_var(&non_isolated_cpus, GFP_KERNEL);
	alloc_cpumask_var(&fallback_doms, GFP_KERNEL);

	sched_init_numa();

	/*
	 * There's no userspace yet to cause hotplug operations; hence all the
	 * cpu masks are stable and all blatant races in the below code cannot
	 * happen.
	 */
	mutex_lock(&sched_domains_mutex);
	init_sched_domains(cpu_active_mask);
	cpumask_andnot(non_isolated_cpus, cpu_possible_mask, cpu_isolated_map);
	if (cpumask_empty(non_isolated_cpus))
		cpumask_set_cpu(smp_processor_id(), non_isolated_cpus);
	mutex_unlock(&sched_domains_mutex);

	hotcpu_notifier(sched_domains_numa_masks_update, CPU_PRI_SCHED_ACTIVE);
	hotcpu_notifier(cpuset_cpu_active, CPU_PRI_CPUSET_ACTIVE);
	hotcpu_notifier(cpuset_cpu_inactive, CPU_PRI_CPUSET_INACTIVE);

	update_cluster_topology();

	init_hrtick();

	/* Move init over to a non-isolated CPU */
	if (set_cpus_allowed_ptr(current, non_isolated_cpus) < 0)
		BUG();
	sched_init_granularity();
	free_cpumask_var(non_isolated_cpus);

	init_sched_rt_class();
	init_sched_dl_class();
}
#else
void __init sched_init_smp(void)
{
	sched_init_granularity();
}
#endif /* CONFIG_SMP */

const_debug unsigned int sysctl_timer_migration = 1;

int in_sched_functions(unsigned long addr)
{
	return in_lock_functions(addr) ||
		(addr >= (unsigned long)__sched_text_start
		&& addr < (unsigned long)__sched_text_end);
}

#ifdef CONFIG_CGROUP_SCHED
/*
 * Default task group.
 * Every task in system belongs to this group at bootup.
 */
struct task_group root_task_group;
LIST_HEAD(task_groups);
#endif

DECLARE_PER_CPU(cpumask_var_t, load_balance_mask);

void __init sched_init(void)
{
	int i, j;
	unsigned long alloc_size = 0, ptr;

	if (sched_enable_hmp)
		pr_info("HMP scheduling enabled.\n");

#ifdef CONFIG_SCHED_HMP
	init_clusters();
#endif

#ifdef CONFIG_FAIR_GROUP_SCHED
	alloc_size += 2 * nr_cpu_ids * sizeof(void **);
#endif
#ifdef CONFIG_RT_GROUP_SCHED
	alloc_size += 2 * nr_cpu_ids * sizeof(void **);
#endif
#ifdef CONFIG_CPUMASK_OFFSTACK
	alloc_size += num_possible_cpus() * cpumask_size();
#endif
	if (alloc_size) {
		ptr = (unsigned long)kzalloc(alloc_size, GFP_NOWAIT);

#ifdef CONFIG_FAIR_GROUP_SCHED
		root_task_group.se = (struct sched_entity **)ptr;
		ptr += nr_cpu_ids * sizeof(void **);

		root_task_group.cfs_rq = (struct cfs_rq **)ptr;
		ptr += nr_cpu_ids * sizeof(void **);

#endif /* CONFIG_FAIR_GROUP_SCHED */
#ifdef CONFIG_RT_GROUP_SCHED
		root_task_group.rt_se = (struct sched_rt_entity **)ptr;
		ptr += nr_cpu_ids * sizeof(void **);

		root_task_group.rt_rq = (struct rt_rq **)ptr;
		ptr += nr_cpu_ids * sizeof(void **);

#endif /* CONFIG_RT_GROUP_SCHED */
#ifdef CONFIG_CPUMASK_OFFSTACK
		for_each_possible_cpu(i) {
			per_cpu(load_balance_mask, i) = (void *)ptr;
			ptr += cpumask_size();
		}
#endif /* CONFIG_CPUMASK_OFFSTACK */
	}

	init_rt_bandwidth(&def_rt_bandwidth,
			global_rt_period(), global_rt_runtime());
	init_dl_bandwidth(&def_dl_bandwidth,
			global_rt_period(), global_rt_runtime());

#ifdef CONFIG_SMP
	init_defrootdomain();
#endif

#ifdef CONFIG_RT_GROUP_SCHED
	init_rt_bandwidth(&root_task_group.rt_bandwidth,
			global_rt_period(), global_rt_runtime());
#endif /* CONFIG_RT_GROUP_SCHED */

#ifdef CONFIG_CGROUP_SCHED
	list_add(&root_task_group.list, &task_groups);
	INIT_LIST_HEAD(&root_task_group.children);
	INIT_LIST_HEAD(&root_task_group.siblings);
	autogroup_init(&init_task);

#endif /* CONFIG_CGROUP_SCHED */

	for_each_possible_cpu(i) {
		struct rq *rq;

		rq = cpu_rq(i);
		raw_spin_lock_init(&rq->lock);
		rq->nr_running = 0;
		rq->calc_load_active = 0;
		rq->calc_load_update = jiffies + LOAD_FREQ;
		init_cfs_rq(&rq->cfs);
		init_rt_rq(&rq->rt, rq);
		init_dl_rq(&rq->dl, rq);
#ifdef CONFIG_FAIR_GROUP_SCHED
		root_task_group.shares = ROOT_TASK_GROUP_LOAD;
		INIT_LIST_HEAD(&rq->leaf_cfs_rq_list);
		/*
		 * How much cpu bandwidth does root_task_group get?
		 *
		 * In case of task-groups formed thr' the cgroup filesystem, it
		 * gets 100% of the cpu resources in the system. This overall
		 * system cpu resource is divided among the tasks of
		 * root_task_group and its child task-groups in a fair manner,
		 * based on each entity's (task or task-group's) weight
		 * (se->load.weight).
		 *
		 * In other words, if root_task_group has 10 tasks of weight
		 * 1024) and two child groups A0 and A1 (of weight 1024 each),
		 * then A0's share of the cpu resource is:
		 *
		 *	A0's bandwidth = 1024 / (10*1024 + 1024 + 1024) = 8.33%
		 *
		 * We achieve this by letting root_task_group's tasks sit
		 * directly in rq->cfs (i.e root_task_group->se[] = NULL).
		 */
		init_cfs_bandwidth(&root_task_group.cfs_bandwidth);
		init_tg_cfs_entry(&root_task_group, &rq->cfs, NULL, i, NULL);
#endif /* CONFIG_FAIR_GROUP_SCHED */

		rq->rt.rt_runtime = def_rt_bandwidth.rt_runtime;
#ifdef CONFIG_RT_GROUP_SCHED
		init_tg_rt_entry(&root_task_group, &rq->rt, NULL, i, NULL);
#endif

		for (j = 0; j < CPU_LOAD_IDX_MAX; j++)
			rq->cpu_load[j] = 0;

		rq->last_load_update_tick = jiffies;

#ifdef CONFIG_SMP
		rq->sd = NULL;
		rq->rd = NULL;
		rq->cpu_capacity = SCHED_CAPACITY_SCALE;
		rq->post_schedule = 0;
		rq->active_balance = 0;
		rq->next_balance = jiffies;
		rq->push_cpu = 0;
		rq->push_task = NULL;
		rq->cpu = i;
		rq->online = 0;
		rq->idle_stamp = 0;
		rq->avg_idle = 2*sysctl_sched_migration_cost;
#ifdef CONFIG_SCHED_HMP
		cpumask_set_cpu(i, &rq->freq_domain_cpumask);
		rq->hmp_stats.cumulative_runnable_avg = 0;
		rq->window_start = 0;
		rq->hmp_stats.nr_big_tasks = 0;
		rq->hmp_flags = 0;
		rq->cur_irqload = 0;
		rq->avg_irqload = 0;
		rq->irqload_ts = 0;
		rq->static_cpu_pwr_cost = 0;
		rq->cc.cycles = SCHED_MIN_FREQ;
		rq->cc.time = 1;

		/*
		 * All cpus part of same cluster by default. This avoids the
		 * need to check for rq->cluster being non-NULL in hot-paths
		 * like select_best_cpu()
		 */
		rq->cluster = &init_cluster;
#ifdef CONFIG_SCHED_FREQ_INPUT
		rq->curr_runnable_sum = rq->prev_runnable_sum = 0;
		rq->nt_curr_runnable_sum = rq->nt_prev_runnable_sum = 0;
		rq->old_busy_time = 0;
		rq->old_estimated_time = 0;
		rq->old_busy_time_group = 0;
		rq->notifier_sent = 0;
		rq->hmp_stats.pred_demands_sum = 0;
#endif
#endif
		rq->max_idle_balance_cost = sysctl_sched_migration_cost;
		rq->cstate = 0;
		rq->wakeup_latency = 0;

		INIT_LIST_HEAD(&rq->cfs_tasks);

		rq_attach_root(rq, &def_root_domain);
#ifdef CONFIG_NO_HZ_COMMON
		rq->nohz_flags = 0;
#endif
#ifdef CONFIG_NO_HZ_FULL
		rq->last_sched_tick = 0;
#endif
#endif
		init_rq_hrtick(rq);
		atomic_set(&rq->nr_iowait, 0);
	}

	set_hmp_defaults();

	set_load_weight(&init_task);

#ifdef CONFIG_PREEMPT_NOTIFIERS
	INIT_HLIST_HEAD(&init_task.preempt_notifiers);
#endif

	/*
	 * The boot idle thread does lazy MMU switching as well:
	 */
	atomic_inc(&init_mm.mm_count);
	enter_lazy_tlb(&init_mm, current);

	/*
	 * Make us the idle thread. Technically, schedule() should not be
	 * called from this thread, however somewhere below it might be,
	 * but because we are the idle thread, we just pick up running again
	 * when this runqueue becomes "idle".
	 */
	init_idle(current, smp_processor_id());

	calc_load_update = jiffies + LOAD_FREQ;

	/*
	 * During early bootup we pretend to be a normal task:
	 */
	current->sched_class = &fair_sched_class;

#ifdef CONFIG_SMP
	zalloc_cpumask_var(&sched_domains_tmpmask, GFP_NOWAIT);
	/* May be allocated at isolcpus cmdline parse time */
	if (cpu_isolated_map == NULL)
		zalloc_cpumask_var(&cpu_isolated_map, GFP_NOWAIT);
	idle_thread_set_boot_cpu();
	set_cpu_rq_start_time();
#endif
	init_sched_fair_class();

	scheduler_running = 1;
}

#ifdef CONFIG_DEBUG_ATOMIC_SLEEP
static inline int preempt_count_equals(int preempt_offset)
{
	int nested = (preempt_count() & ~PREEMPT_ACTIVE) + rcu_preempt_depth();

	return (nested == preempt_offset);
}

static int __might_sleep_init_called;
int __init __might_sleep_init(void)
{
	__might_sleep_init_called = 1;
	return 0;
}
early_initcall(__might_sleep_init);

void __might_sleep(const char *file, int line, int preempt_offset)
{
	static unsigned long prev_jiffy;	/* ratelimiting */

	rcu_sleep_check(); /* WARN_ON_ONCE() by default, no rate limit reqd. */
	if ((preempt_count_equals(preempt_offset) && !irqs_disabled() &&
	     !is_idle_task(current)) || oops_in_progress)
		return;
	if (system_state != SYSTEM_RUNNING &&
	    (!__might_sleep_init_called || system_state != SYSTEM_BOOTING))
		return;
	if (time_before(jiffies, prev_jiffy + HZ) && prev_jiffy)
		return;
	prev_jiffy = jiffies;

	printk(KERN_ERR
		"BUG: sleeping function called from invalid context at %s:%d\n",
			file, line);
	printk(KERN_ERR
		"in_atomic(): %d, irqs_disabled(): %d, pid: %d, name: %s\n",
			in_atomic(), irqs_disabled(),
			current->pid, current->comm);

	debug_show_held_locks(current);
	if (irqs_disabled())
		print_irqtrace_events(current);
#ifdef CONFIG_DEBUG_PREEMPT
	if (!preempt_count_equals(preempt_offset)) {
		pr_err("Preemption disabled at:");
		print_ip_sym(current->preempt_disable_ip);
		pr_cont("\n");
	}
#endif
#ifdef CONFIG_PANIC_ON_SCHED_BUG
	BUG();
#endif
	dump_stack();
}
EXPORT_SYMBOL(__might_sleep);
#endif

#ifdef CONFIG_MAGIC_SYSRQ
static void normalize_task(struct rq *rq, struct task_struct *p)
{
	const struct sched_class *prev_class = p->sched_class;
	struct sched_attr attr = {
		.sched_policy = SCHED_NORMAL,
	};
	int old_prio = p->prio;
	int queued;

	queued = task_on_rq_queued(p);
	if (queued)
		dequeue_task(rq, p, 0);
	__setscheduler(rq, p, &attr, false);
	if (queued) {
		enqueue_task(rq, p, 0);
		resched_curr(rq);
	}

	check_class_changed(rq, p, prev_class, old_prio);
}

void normalize_rt_tasks(void)
{
	struct task_struct *g, *p;
	unsigned long flags;
	struct rq *rq;

	read_lock(&tasklist_lock);
	for_each_process_thread(g, p) {
		/*
		 * Only normalize user tasks:
		 */
		if (p->flags & PF_KTHREAD)
			continue;

		p->se.exec_start		= 0;
#ifdef CONFIG_SCHEDSTATS
		p->se.statistics.wait_start	= 0;
		p->se.statistics.sleep_start	= 0;
		p->se.statistics.block_start	= 0;
#endif

		if (!dl_task(p) && !rt_task(p)) {
			/*
			 * Renice negative nice level userspace
			 * tasks back to 0:
			 */
			if (task_nice(p) < 0)
				set_user_nice(p, 0);
			continue;
		}

		rq = task_rq_lock(p, &flags);
		normalize_task(rq, p);
		task_rq_unlock(rq, p, &flags);
	}
	read_unlock(&tasklist_lock);
}

#endif /* CONFIG_MAGIC_SYSRQ */

#if defined(CONFIG_IA64) || defined(CONFIG_KGDB_KDB)
/*
 * These functions are only useful for the IA64 MCA handling, or kdb.
 *
 * They can only be called when the whole system has been
 * stopped - every CPU needs to be quiescent, and no scheduling
 * activity can take place. Using them for anything else would
 * be a serious bug, and as a result, they aren't even visible
 * under any other configuration.
 */

/**
 * curr_task - return the current task for a given cpu.
 * @cpu: the processor in question.
 *
 * ONLY VALID WHEN THE WHOLE SYSTEM IS STOPPED!
 *
 * Return: The current task for @cpu.
 */
struct task_struct *curr_task(int cpu)
{
	return cpu_curr(cpu);
}

#endif /* defined(CONFIG_IA64) || defined(CONFIG_KGDB_KDB) */

#ifdef CONFIG_IA64
/**
 * set_curr_task - set the current task for a given cpu.
 * @cpu: the processor in question.
 * @p: the task pointer to set.
 *
 * Description: This function must only be used when non-maskable interrupts
 * are serviced on a separate stack. It allows the architecture to switch the
 * notion of the current task on a cpu in a non-blocking manner. This function
 * must be called with all CPU's synchronized, and interrupts disabled, the
 * and caller must save the original value of the current task (see
 * curr_task() above) and restore that value before reenabling interrupts and
 * re-starting the system.
 *
 * ONLY VALID WHEN THE WHOLE SYSTEM IS STOPPED!
 */
void set_curr_task(int cpu, struct task_struct *p)
{
	cpu_curr(cpu) = p;
}

#endif

#ifdef CONFIG_CGROUP_SCHED
/* task_group_lock serializes the addition/removal of task groups */
static DEFINE_SPINLOCK(task_group_lock);

static void free_sched_group(struct task_group *tg)
{
	free_fair_sched_group(tg);
	free_rt_sched_group(tg);
	autogroup_free(tg);
	kfree(tg);
}

/* allocate runqueue etc for a new task group */
struct task_group *sched_create_group(struct task_group *parent)
{
	struct task_group *tg;

	tg = kzalloc(sizeof(*tg), GFP_KERNEL);
	if (!tg)
		return ERR_PTR(-ENOMEM);

	if (!alloc_fair_sched_group(tg, parent))
		goto err;

	if (!alloc_rt_sched_group(tg, parent))
		goto err;

	return tg;

err:
	free_sched_group(tg);
	return ERR_PTR(-ENOMEM);
}

void sched_online_group(struct task_group *tg, struct task_group *parent)
{
	unsigned long flags;

	spin_lock_irqsave(&task_group_lock, flags);
	list_add_rcu(&tg->list, &task_groups);

	WARN_ON(!parent); /* root should already exist */

	tg->parent = parent;
	INIT_LIST_HEAD(&tg->children);
	list_add_rcu(&tg->siblings, &parent->children);
	spin_unlock_irqrestore(&task_group_lock, flags);
}

/* rcu callback to free various structures associated with a task group */
static void free_sched_group_rcu(struct rcu_head *rhp)
{
	/* now it should be safe to free those cfs_rqs */
	free_sched_group(container_of(rhp, struct task_group, rcu));
}

/* Destroy runqueue etc associated with a task group */
void sched_destroy_group(struct task_group *tg)
{
	/* wait for possible concurrent references to cfs_rqs complete */
	call_rcu(&tg->rcu, free_sched_group_rcu);
}

void sched_offline_group(struct task_group *tg)
{
	unsigned long flags;
	int i;

	/* end participation in shares distribution */
	for_each_possible_cpu(i)
		unregister_fair_sched_group(tg, i);

	spin_lock_irqsave(&task_group_lock, flags);
	list_del_rcu(&tg->list);
	list_del_rcu(&tg->siblings);
	spin_unlock_irqrestore(&task_group_lock, flags);
}

/* change task's runqueue when it moves between groups.
 *	The caller of this function should have put the task in its new group
 *	by now. This function just updates tsk->se.cfs_rq and tsk->se.parent to
 *	reflect its new group.
 */
void sched_move_task(struct task_struct *tsk)
{
	struct task_group *tg;
	int queued, running;
	unsigned long flags;
	struct rq *rq;

	rq = task_rq_lock(tsk, &flags);

	running = task_current(rq, tsk);
	queued = task_on_rq_queued(tsk);

	if (queued)
		dequeue_task(rq, tsk, DEQUEUE_SAVE | DEQUEUE_MOVE);
	if (unlikely(running))
		put_prev_task(rq, tsk);

	/*
	 * All callers are synchronized by task_rq_lock(); we do not use RCU
	 * which is pointless here. Thus, we pass "true" to task_css_check()
	 * to prevent lockdep warnings.
	 */
	tg = container_of(task_css_check(tsk, cpu_cgrp_id, true),
			  struct task_group, css);
	tg = autogroup_task_group(tsk, tg);
	tsk->sched_task_group = tg;

#ifdef CONFIG_FAIR_GROUP_SCHED
	if (tsk->sched_class->task_move_group)
		tsk->sched_class->task_move_group(tsk, queued);
	else
#endif
		set_task_rq(tsk, task_cpu(tsk));

	if (unlikely(running))
		tsk->sched_class->set_curr_task(rq);
	if (queued)
		enqueue_task(rq, tsk, ENQUEUE_RESTORE | ENQUEUE_MOVE);

	task_rq_unlock(rq, tsk, &flags);
}
#endif /* CONFIG_CGROUP_SCHED */

#ifdef CONFIG_RT_GROUP_SCHED
/*
 * Ensure that the real time constraints are schedulable.
 */
static DEFINE_MUTEX(rt_constraints_mutex);

/* Must be called with tasklist_lock held */
static inline int tg_has_rt_tasks(struct task_group *tg)
{
	struct task_struct *g, *p;

	/*
	 * Autogroups do not have RT tasks; see autogroup_create().
	 */
	if (task_group_is_autogroup(tg))
		return 0;

	for_each_process_thread(g, p) {
		if (rt_task(p) && task_group(p) == tg)
			return 1;
	}

	return 0;
}

struct rt_schedulable_data {
	struct task_group *tg;
	u64 rt_period;
	u64 rt_runtime;
};

static int tg_rt_schedulable(struct task_group *tg, void *data)
{
	struct rt_schedulable_data *d = data;
	struct task_group *child;
	unsigned long total, sum = 0;
	u64 period, runtime;

	period = ktime_to_ns(tg->rt_bandwidth.rt_period);
	runtime = tg->rt_bandwidth.rt_runtime;

	if (tg == d->tg) {
		period = d->rt_period;
		runtime = d->rt_runtime;
	}

	/*
	 * Cannot have more runtime than the period.
	 */
	if (runtime > period && runtime != RUNTIME_INF)
		return -EINVAL;

	/*
	 * Ensure we don't starve existing RT tasks.
	 */
	if (rt_bandwidth_enabled() && !runtime && tg_has_rt_tasks(tg))
		return -EBUSY;

	total = to_ratio(period, runtime);

	/*
	 * Nobody can have more than the global setting allows.
	 */
	if (total > to_ratio(global_rt_period(), global_rt_runtime()))
		return -EINVAL;

	/*
	 * The sum of our children's runtime should not exceed our own.
	 */
	list_for_each_entry_rcu(child, &tg->children, siblings) {
		period = ktime_to_ns(child->rt_bandwidth.rt_period);
		runtime = child->rt_bandwidth.rt_runtime;

		if (child == d->tg) {
			period = d->rt_period;
			runtime = d->rt_runtime;
		}

		sum += to_ratio(period, runtime);
	}

	if (sum > total)
		return -EINVAL;

	return 0;
}

static int __rt_schedulable(struct task_group *tg, u64 period, u64 runtime)
{
	int ret;

	struct rt_schedulable_data data = {
		.tg = tg,
		.rt_period = period,
		.rt_runtime = runtime,
	};

	rcu_read_lock();
	ret = walk_tg_tree(tg_rt_schedulable, tg_nop, &data);
	rcu_read_unlock();

	return ret;
}

static int tg_set_rt_bandwidth(struct task_group *tg,
		u64 rt_period, u64 rt_runtime)
{
	int i, err = 0;

	mutex_lock(&rt_constraints_mutex);
	read_lock(&tasklist_lock);
	err = __rt_schedulable(tg, rt_period, rt_runtime);
	if (err)
		goto unlock;

	raw_spin_lock_irq(&tg->rt_bandwidth.rt_runtime_lock);
	tg->rt_bandwidth.rt_period = ns_to_ktime(rt_period);
	tg->rt_bandwidth.rt_runtime = rt_runtime;

	for_each_possible_cpu(i) {
		struct rt_rq *rt_rq = tg->rt_rq[i];

		raw_spin_lock(&rt_rq->rt_runtime_lock);
		rt_rq->rt_runtime = rt_runtime;
		raw_spin_unlock(&rt_rq->rt_runtime_lock);
	}
	raw_spin_unlock_irq(&tg->rt_bandwidth.rt_runtime_lock);
unlock:
	read_unlock(&tasklist_lock);
	mutex_unlock(&rt_constraints_mutex);

	return err;
}

static int sched_group_set_rt_runtime(struct task_group *tg, long rt_runtime_us)
{
	u64 rt_runtime, rt_period;

	rt_period = ktime_to_ns(tg->rt_bandwidth.rt_period);
	rt_runtime = (u64)rt_runtime_us * NSEC_PER_USEC;
	if (rt_runtime_us < 0)
		rt_runtime = RUNTIME_INF;

	return tg_set_rt_bandwidth(tg, rt_period, rt_runtime);
}

static long sched_group_rt_runtime(struct task_group *tg)
{
	u64 rt_runtime_us;

	if (tg->rt_bandwidth.rt_runtime == RUNTIME_INF)
		return -1;

	rt_runtime_us = tg->rt_bandwidth.rt_runtime;
	do_div(rt_runtime_us, NSEC_PER_USEC);
	return rt_runtime_us;
}

static int sched_group_set_rt_period(struct task_group *tg, long rt_period_us)
{
	u64 rt_runtime, rt_period;

	rt_period = (u64)rt_period_us * NSEC_PER_USEC;
	rt_runtime = tg->rt_bandwidth.rt_runtime;

	if (rt_period == 0)
		return -EINVAL;

	return tg_set_rt_bandwidth(tg, rt_period, rt_runtime);
}

static long sched_group_rt_period(struct task_group *tg)
{
	u64 rt_period_us;

	rt_period_us = ktime_to_ns(tg->rt_bandwidth.rt_period);
	do_div(rt_period_us, NSEC_PER_USEC);
	return rt_period_us;
}
#endif /* CONFIG_RT_GROUP_SCHED */

#ifdef CONFIG_RT_GROUP_SCHED
static int sched_rt_global_constraints(void)
{
	int ret = 0;

	mutex_lock(&rt_constraints_mutex);
	read_lock(&tasklist_lock);
	ret = __rt_schedulable(NULL, 0, 0);
	read_unlock(&tasklist_lock);
	mutex_unlock(&rt_constraints_mutex);

	return ret;
}

static int sched_rt_can_attach(struct task_group *tg, struct task_struct *tsk)
{
	/* Don't accept realtime tasks when there is no way for them to run */
	if (rt_task(tsk) && tg->rt_bandwidth.rt_runtime == 0)
		return 0;

	return 1;
}

#else /* !CONFIG_RT_GROUP_SCHED */
static int sched_rt_global_constraints(void)
{
	unsigned long flags;
	int i, ret = 0;

	raw_spin_lock_irqsave(&def_rt_bandwidth.rt_runtime_lock, flags);
	for_each_possible_cpu(i) {
		struct rt_rq *rt_rq = &cpu_rq(i)->rt;

		raw_spin_lock(&rt_rq->rt_runtime_lock);
		rt_rq->rt_runtime = global_rt_runtime();
		raw_spin_unlock(&rt_rq->rt_runtime_lock);
	}
	raw_spin_unlock_irqrestore(&def_rt_bandwidth.rt_runtime_lock, flags);

	return ret;
}
#endif /* CONFIG_RT_GROUP_SCHED */

static int sched_dl_global_constraints(void)
{
	u64 runtime = global_rt_runtime();
	u64 period = global_rt_period();
	u64 new_bw = to_ratio(period, runtime);
	struct dl_bw *dl_b;
	int cpu, ret = 0;
	unsigned long flags;

	/*
	 * Here we want to check the bandwidth not being set to some
	 * value smaller than the currently allocated bandwidth in
	 * any of the root_domains.
	 *
	 * FIXME: Cycling on all the CPUs is overdoing, but simpler than
	 * cycling on root_domains... Discussion on different/better
	 * solutions is welcome!
	 */
	for_each_possible_cpu(cpu) {
		rcu_read_lock_sched();
		dl_b = dl_bw_of(cpu);

		raw_spin_lock_irqsave(&dl_b->lock, flags);
		if (new_bw < dl_b->total_bw)
			ret = -EBUSY;
		raw_spin_unlock_irqrestore(&dl_b->lock, flags);

		rcu_read_unlock_sched();

		if (ret)
			break;
	}

	return ret;
}

static void sched_dl_do_global(void)
{
	u64 new_bw = -1;
	struct dl_bw *dl_b;
	int cpu;
	unsigned long flags;

	def_dl_bandwidth.dl_period = global_rt_period();
	def_dl_bandwidth.dl_runtime = global_rt_runtime();

	if (global_rt_runtime() != RUNTIME_INF)
		new_bw = to_ratio(global_rt_period(), global_rt_runtime());

	/*
	 * FIXME: As above...
	 */
	for_each_possible_cpu(cpu) {
		rcu_read_lock_sched();
		dl_b = dl_bw_of(cpu);

		raw_spin_lock_irqsave(&dl_b->lock, flags);
		dl_b->bw = new_bw;
		raw_spin_unlock_irqrestore(&dl_b->lock, flags);

		rcu_read_unlock_sched();
	}
}

static int sched_rt_global_validate(void)
{
	if (sysctl_sched_rt_period <= 0)
		return -EINVAL;

	if ((sysctl_sched_rt_runtime != RUNTIME_INF) &&
		(sysctl_sched_rt_runtime > sysctl_sched_rt_period))
		return -EINVAL;

	return 0;
}

static void sched_rt_do_global(void)
{
	def_rt_bandwidth.rt_runtime = global_rt_runtime();
	def_rt_bandwidth.rt_period = ns_to_ktime(global_rt_period());
}

int sched_rt_handler(struct ctl_table *table, int write,
		void __user *buffer, size_t *lenp,
		loff_t *ppos)
{
	int old_period, old_runtime;
	static DEFINE_MUTEX(mutex);
	int ret;

	mutex_lock(&mutex);
	old_period = sysctl_sched_rt_period;
	old_runtime = sysctl_sched_rt_runtime;

	ret = proc_dointvec(table, write, buffer, lenp, ppos);

	if (!ret && write) {
		ret = sched_rt_global_validate();
		if (ret)
			goto undo;

		ret = sched_rt_global_constraints();
		if (ret)
			goto undo;

		ret = sched_dl_global_constraints();
		if (ret)
			goto undo;

		sched_rt_do_global();
		sched_dl_do_global();
	}
	if (0) {
undo:
		sysctl_sched_rt_period = old_period;
		sysctl_sched_rt_runtime = old_runtime;
	}
	mutex_unlock(&mutex);

	return ret;
}

int sched_rr_handler(struct ctl_table *table, int write,
		void __user *buffer, size_t *lenp,
		loff_t *ppos)
{
	int ret;
	static DEFINE_MUTEX(mutex);

	mutex_lock(&mutex);
	ret = proc_dointvec(table, write, buffer, lenp, ppos);
	/* make sure that internally we keep jiffies */
	/* also, writing zero resets timeslice to default */
	if (!ret && write) {
		sched_rr_timeslice = sched_rr_timeslice <= 0 ?
			RR_TIMESLICE : msecs_to_jiffies(sched_rr_timeslice);
	}
	mutex_unlock(&mutex);
	return ret;
}

#ifdef CONFIG_CGROUP_SCHED

static inline struct task_group *css_tg(struct cgroup_subsys_state *css)
{
	return css ? container_of(css, struct task_group, css) : NULL;
}

static struct cgroup_subsys_state *
cpu_cgroup_css_alloc(struct cgroup_subsys_state *parent_css)
{
	struct task_group *parent = css_tg(parent_css);
	struct task_group *tg;

	if (!parent) {
		/* This is early initialization for the top cgroup */
		return &root_task_group.css;
	}

	tg = sched_create_group(parent);
	if (IS_ERR(tg))
		return ERR_PTR(-ENOMEM);

	return &tg->css;
}

static int cpu_cgroup_css_online(struct cgroup_subsys_state *css)
{
	struct task_group *tg = css_tg(css);
	struct task_group *parent = css_tg(css->parent);

	if (parent)
		sched_online_group(tg, parent);
	return 0;
}

static void cpu_cgroup_css_free(struct cgroup_subsys_state *css)
{
	struct task_group *tg = css_tg(css);

	sched_destroy_group(tg);
}

static void cpu_cgroup_css_offline(struct cgroup_subsys_state *css)
{
	struct task_group *tg = css_tg(css);

	sched_offline_group(tg);
}

static void cpu_cgroup_fork(struct task_struct *task)
{
	sched_move_task(task);
}

static int cpu_cgroup_can_attach(struct cgroup_subsys_state *css,
				 struct cgroup_taskset *tset)
{
	struct task_struct *task;

	cgroup_taskset_for_each(task, tset) {
#ifdef CONFIG_RT_GROUP_SCHED
		if (!sched_rt_can_attach(css_tg(css), task))
			return -EINVAL;
#else
		/* We don't support RT-tasks being in separate groups */
		if (task->sched_class != &fair_sched_class)
			return -EINVAL;
#endif
	}
	return 0;
}

static void cpu_cgroup_attach(struct cgroup_subsys_state *css,
			      struct cgroup_taskset *tset)
{
	struct task_struct *task;

	cgroup_taskset_for_each(task, tset)
		sched_move_task(task);
}

static void cpu_cgroup_exit(struct cgroup_subsys_state *css,
			    struct cgroup_subsys_state *old_css,
			    struct task_struct *task)
{
	/*
	 * cgroup_exit() is called in the copy_process() failure path.
	 * Ignore this case since the task hasn't ran yet, this avoids
	 * trying to poke a half freed task state from generic code.
	 */
	if (!(task->flags & PF_EXITING))
		return;

	sched_move_task(task);
}

static u64 cpu_notify_on_migrate_read_u64(struct cgroup_subsys_state *css,
					  struct cftype *cft)
{
	struct task_group *tg = css_tg(css);

	return tg->notify_on_migrate;
}

static int cpu_notify_on_migrate_write_u64(struct cgroup_subsys_state *css,
					   struct cftype *cft, u64 notify)
{
	struct task_group *tg = css_tg(css);

	tg->notify_on_migrate = (notify > 0);

	return 0;
}

#ifdef CONFIG_SCHED_HMP

static u64 cpu_upmigrate_discourage_read_u64(struct cgroup_subsys_state *css,
					  struct cftype *cft)
{
	struct task_group *tg = css_tg(css);

	return tg->upmigrate_discouraged;
}

static int cpu_upmigrate_discourage_write_u64(struct cgroup_subsys_state *css,
				struct cftype *cft, u64 upmigrate_discourage)
{
	struct task_group *tg = css_tg(css);
	int discourage = upmigrate_discourage > 0;

	if (tg->upmigrate_discouraged == discourage)
		return 0;

	/*
	 * Revisit big-task classification for tasks of this cgroup. It would
	 * have been efficient to walk tasks of just this cgroup in running
	 * state, but we don't have easy means to do that. Walk all tasks in
	 * running state on all cpus instead and re-visit their big task
	 * classification.
	 */
	get_online_cpus();
	pre_big_task_count_change(cpu_online_mask);

	tg->upmigrate_discouraged = discourage;

	post_big_task_count_change(cpu_online_mask);
	put_online_cpus();

	return 0;
}

#endif	/* CONFIG_SCHED_HMP */

#ifdef CONFIG_FAIR_GROUP_SCHED
static int cpu_shares_write_u64(struct cgroup_subsys_state *css,
				struct cftype *cftype, u64 shareval)
{
	return sched_group_set_shares(css_tg(css), scale_load(shareval));
}

static u64 cpu_shares_read_u64(struct cgroup_subsys_state *css,
			       struct cftype *cft)
{
	struct task_group *tg = css_tg(css);

	return (u64) scale_load_down(tg->shares);
}

#ifdef CONFIG_CFS_BANDWIDTH
static DEFINE_MUTEX(cfs_constraints_mutex);

const u64 max_cfs_quota_period = 1 * NSEC_PER_SEC; /* 1s */
const u64 min_cfs_quota_period = 1 * NSEC_PER_MSEC; /* 1ms */

static int __cfs_schedulable(struct task_group *tg, u64 period, u64 runtime);

static int tg_set_cfs_bandwidth(struct task_group *tg, u64 period, u64 quota)
{
	int i, ret = 0, runtime_enabled, runtime_was_enabled;
	struct cfs_bandwidth *cfs_b = &tg->cfs_bandwidth;

	if (tg == &root_task_group)
		return -EINVAL;

	/*
	 * Ensure we have at some amount of bandwidth every period.  This is
	 * to prevent reaching a state of large arrears when throttled via
	 * entity_tick() resulting in prolonged exit starvation.
	 */
	if (quota < min_cfs_quota_period || period < min_cfs_quota_period)
		return -EINVAL;

	/*
	 * Likewise, bound things on the otherside by preventing insane quota
	 * periods.  This also allows us to normalize in computing quota
	 * feasibility.
	 */
	if (period > max_cfs_quota_period)
		return -EINVAL;

	/*
	 * Prevent race between setting of cfs_rq->runtime_enabled and
	 * unthrottle_offline_cfs_rqs().
	 */
	get_online_cpus();
	mutex_lock(&cfs_constraints_mutex);
	ret = __cfs_schedulable(tg, period, quota);
	if (ret)
		goto out_unlock;

	runtime_enabled = quota != RUNTIME_INF;
	runtime_was_enabled = cfs_b->quota != RUNTIME_INF;
	/*
	 * If we need to toggle cfs_bandwidth_used, off->on must occur
	 * before making related changes, and on->off must occur afterwards
	 */
	if (runtime_enabled && !runtime_was_enabled)
		cfs_bandwidth_usage_inc();
	raw_spin_lock_irq(&cfs_b->lock);
	cfs_b->period = ns_to_ktime(period);
	cfs_b->quota = quota;

	__refill_cfs_bandwidth_runtime(cfs_b);
	/* restart the period timer (if active) to handle new period expiry */
	if (runtime_enabled && cfs_b->timer_active) {
		/* force a reprogram */
		__start_cfs_bandwidth(cfs_b, true);
	}
	raw_spin_unlock_irq(&cfs_b->lock);

	for_each_online_cpu(i) {
		struct cfs_rq *cfs_rq = tg->cfs_rq[i];
		struct rq *rq = cfs_rq->rq;

		raw_spin_lock_irq(&rq->lock);
		cfs_rq->runtime_enabled = runtime_enabled;
		cfs_rq->runtime_remaining = 0;

		if (cfs_rq->throttled)
			unthrottle_cfs_rq(cfs_rq);
		raw_spin_unlock_irq(&rq->lock);
	}
	if (runtime_was_enabled && !runtime_enabled)
		cfs_bandwidth_usage_dec();
out_unlock:
	mutex_unlock(&cfs_constraints_mutex);
	put_online_cpus();

	return ret;
}

int tg_set_cfs_quota(struct task_group *tg, long cfs_quota_us)
{
	u64 quota, period;

	period = ktime_to_ns(tg->cfs_bandwidth.period);
	if (cfs_quota_us < 0)
		quota = RUNTIME_INF;
	else
		quota = (u64)cfs_quota_us * NSEC_PER_USEC;

	return tg_set_cfs_bandwidth(tg, period, quota);
}

long tg_get_cfs_quota(struct task_group *tg)
{
	u64 quota_us;

	if (tg->cfs_bandwidth.quota == RUNTIME_INF)
		return -1;

	quota_us = tg->cfs_bandwidth.quota;
	do_div(quota_us, NSEC_PER_USEC);

	return quota_us;
}

int tg_set_cfs_period(struct task_group *tg, long cfs_period_us)
{
	u64 quota, period;

	period = (u64)cfs_period_us * NSEC_PER_USEC;
	quota = tg->cfs_bandwidth.quota;

	return tg_set_cfs_bandwidth(tg, period, quota);
}

long tg_get_cfs_period(struct task_group *tg)
{
	u64 cfs_period_us;

	cfs_period_us = ktime_to_ns(tg->cfs_bandwidth.period);
	do_div(cfs_period_us, NSEC_PER_USEC);

	return cfs_period_us;
}

static s64 cpu_cfs_quota_read_s64(struct cgroup_subsys_state *css,
				  struct cftype *cft)
{
	return tg_get_cfs_quota(css_tg(css));
}

static int cpu_cfs_quota_write_s64(struct cgroup_subsys_state *css,
				   struct cftype *cftype, s64 cfs_quota_us)
{
	return tg_set_cfs_quota(css_tg(css), cfs_quota_us);
}

static u64 cpu_cfs_period_read_u64(struct cgroup_subsys_state *css,
				   struct cftype *cft)
{
	return tg_get_cfs_period(css_tg(css));
}

static int cpu_cfs_period_write_u64(struct cgroup_subsys_state *css,
				    struct cftype *cftype, u64 cfs_period_us)
{
	return tg_set_cfs_period(css_tg(css), cfs_period_us);
}

struct cfs_schedulable_data {
	struct task_group *tg;
	u64 period, quota;
};

/*
 * normalize group quota/period to be quota/max_period
 * note: units are usecs
 */
static u64 normalize_cfs_quota(struct task_group *tg,
			       struct cfs_schedulable_data *d)
{
	u64 quota, period;

	if (tg == d->tg) {
		period = d->period;
		quota = d->quota;
	} else {
		period = tg_get_cfs_period(tg);
		quota = tg_get_cfs_quota(tg);
	}

	/* note: these should typically be equivalent */
	if (quota == RUNTIME_INF || quota == -1)
		return RUNTIME_INF;

	return to_ratio(period, quota);
}

static int tg_cfs_schedulable_down(struct task_group *tg, void *data)
{
	struct cfs_schedulable_data *d = data;
	struct cfs_bandwidth *cfs_b = &tg->cfs_bandwidth;
	s64 quota = 0, parent_quota = -1;

	if (!tg->parent) {
		quota = RUNTIME_INF;
	} else {
		struct cfs_bandwidth *parent_b = &tg->parent->cfs_bandwidth;

		quota = normalize_cfs_quota(tg, d);
		parent_quota = parent_b->hierarchical_quota;

		/*
		 * ensure max(child_quota) <= parent_quota, inherit when no
		 * limit is set
		 */
		if (quota == RUNTIME_INF)
			quota = parent_quota;
		else if (parent_quota != RUNTIME_INF && quota > parent_quota)
			return -EINVAL;
	}
	cfs_b->hierarchical_quota = quota;

	return 0;
}

static int __cfs_schedulable(struct task_group *tg, u64 period, u64 quota)
{
	int ret;
	struct cfs_schedulable_data data = {
		.tg = tg,
		.period = period,
		.quota = quota,
	};

	if (quota != RUNTIME_INF) {
		do_div(data.period, NSEC_PER_USEC);
		do_div(data.quota, NSEC_PER_USEC);
	}

	rcu_read_lock();
	ret = walk_tg_tree(tg_cfs_schedulable_down, tg_nop, &data);
	rcu_read_unlock();

	return ret;
}

static int cpu_stats_show(struct seq_file *sf, void *v)
{
	struct task_group *tg = css_tg(seq_css(sf));
	struct cfs_bandwidth *cfs_b = &tg->cfs_bandwidth;

	seq_printf(sf, "nr_periods %d\n", cfs_b->nr_periods);
	seq_printf(sf, "nr_throttled %d\n", cfs_b->nr_throttled);
	seq_printf(sf, "throttled_time %llu\n", cfs_b->throttled_time);

	return 0;
}
#endif /* CONFIG_CFS_BANDWIDTH */
#endif /* CONFIG_FAIR_GROUP_SCHED */

#ifdef CONFIG_RT_GROUP_SCHED
static int cpu_rt_runtime_write(struct cgroup_subsys_state *css,
				struct cftype *cft, s64 val)
{
	return sched_group_set_rt_runtime(css_tg(css), val);
}

static s64 cpu_rt_runtime_read(struct cgroup_subsys_state *css,
			       struct cftype *cft)
{
	return sched_group_rt_runtime(css_tg(css));
}

static int cpu_rt_period_write_uint(struct cgroup_subsys_state *css,
				    struct cftype *cftype, u64 rt_period_us)
{
	return sched_group_set_rt_period(css_tg(css), rt_period_us);
}

static u64 cpu_rt_period_read_uint(struct cgroup_subsys_state *css,
				   struct cftype *cft)
{
	return sched_group_rt_period(css_tg(css));
}
#endif /* CONFIG_RT_GROUP_SCHED */

static struct cftype cpu_files[] = {
	{
		.name = "notify_on_migrate",
		.read_u64 = cpu_notify_on_migrate_read_u64,
		.write_u64 = cpu_notify_on_migrate_write_u64,
	},
#ifdef CONFIG_SCHED_HMP
	{
		.name = "upmigrate_discourage",
		.read_u64 = cpu_upmigrate_discourage_read_u64,
		.write_u64 = cpu_upmigrate_discourage_write_u64,
	},
#endif
#ifdef CONFIG_FAIR_GROUP_SCHED
	{
		.name = "shares",
		.read_u64 = cpu_shares_read_u64,
		.write_u64 = cpu_shares_write_u64,
	},
#endif
#ifdef CONFIG_CFS_BANDWIDTH
	{
		.name = "cfs_quota_us",
		.read_s64 = cpu_cfs_quota_read_s64,
		.write_s64 = cpu_cfs_quota_write_s64,
	},
	{
		.name = "cfs_period_us",
		.read_u64 = cpu_cfs_period_read_u64,
		.write_u64 = cpu_cfs_period_write_u64,
	},
	{
		.name = "stat",
		.seq_show = cpu_stats_show,
	},
#endif
#ifdef CONFIG_RT_GROUP_SCHED
	{
		.name = "rt_runtime_us",
		.read_s64 = cpu_rt_runtime_read,
		.write_s64 = cpu_rt_runtime_write,
	},
	{
		.name = "rt_period_us",
		.read_u64 = cpu_rt_period_read_uint,
		.write_u64 = cpu_rt_period_write_uint,
	},
#endif
	{ }	/* terminate */
};

struct cgroup_subsys cpu_cgrp_subsys = {
	.css_alloc	= cpu_cgroup_css_alloc,
	.css_free	= cpu_cgroup_css_free,
	.css_online	= cpu_cgroup_css_online,
	.css_offline	= cpu_cgroup_css_offline,
	.fork		= cpu_cgroup_fork,
	.can_attach	= cpu_cgroup_can_attach,
	.attach		= cpu_cgroup_attach,
	.allow_attach   = subsys_cgroup_allow_attach,
	.exit		= cpu_cgroup_exit,
	.legacy_cftypes	= cpu_files,
	.early_init	= 1,
};

#endif	/* CONFIG_CGROUP_SCHED */

void dump_cpu_task(int cpu)
{
	pr_info("Task dump for CPU %d:\n", cpu);
	sched_show_task(cpu_curr(cpu));
}
