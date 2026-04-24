#include "hmbird_sched.h"

#define HMBIRD_SCHED_DIR        "hmbird_cfg"
#define HIGHRES_TICK_CTRL       "scx_shadow_tick_enable"
#define HIGHRES_TICK_CTRL_DBG   "highres_tick_ctrl_dbg"
#define LOAD_TRACK_DIR          "slim_walt"
#define SLIM_WALT_CTRL          "slim_walt_ctrl"
#define SLIM_WALT_DUMP          "slim_walt_dump"
#define SLIM_WALT_POLICY        "slim_walt_policy"
#define SLIM_RAVG_WINDOW        "frame_per_sec"
#define SLIM_FREQ_GOV_DEBUG     "slim_gov_debug"
#define SLIM_FREQ_GOV_DIR       "slim_freq_gov"
#define CPU7_TL                 "cpu7_tl"

#define SCX_GOV_CTRL		"scx_gov_ctrl"

unsigned int highres_tick_ctrl;
unsigned int highres_tick_ctrl_dbg;

int slim_walt_ctrl;
int slim_walt_dump;
int slim_walt_policy;
int sched_ravg_window_frame_per_sec = 125;
int slim_gov_debug;
int cpu7_tl = 70;

int scx_gov_ctrl = 1;
extern struct yield_opt_params yield_opt_params;
#ifdef CONFIG_ARCH_MEDIATEK
extern struct boost_policy_params boost_policy_params;
extern struct tick_hit_params tick_hit_params;
#endif

static char *slim_walt_config[] = {
	SLIM_WALT_CTRL,
	SLIM_WALT_DUMP,
	SLIM_WALT_POLICY,
	SLIM_RAVG_WINDOW,
};

static char *slim_freq_gov_config[] = {
	SLIM_FREQ_GOV_DEBUG,
	SCX_GOV_CTRL,
};

static int *slim_freq_gov_data[] = {
	&slim_gov_debug,
	&scx_gov_ctrl,
};

static int *slim_walt_data[] = {
	&slim_walt_ctrl,
	&slim_walt_dump,
	&slim_walt_policy,
	&sched_ravg_window_frame_per_sec,
};

static char *files_name[] = {
	HIGHRES_TICK_CTRL,
	HIGHRES_TICK_CTRL_DBG,
	CPU7_TL,
};

static int *file_data[] = {
	&highres_tick_ctrl,
	&highres_tick_ctrl_dbg,
	&cpu7_tl,
};

static ssize_t hmbird_common_write(struct file *file,
				   const char __user *buf,
				   size_t count, loff_t *ppos)
{
	int *pval = (int *)pde_data(file_inode(file));
	char kbuf[5] = {0};
	int err;

	if (count >= 5)
		return -EFAULT;

	if (copy_from_user(kbuf, buf, count)) {
		pr_err("hmbird_sched : Failed to copy_from_user\n");
		return -EFAULT;
	}
	err = kstrtoint(strstrip(kbuf), 0, pval);
	if (err < 0) {
		pr_err("hmbird_sched: Failed to exec kstrtoint\n");
		return -EFAULT;
	}

	if (pval == &sched_ravg_window_frame_per_sec) {
#ifdef CONFIG_SCX_USE_UTIL_TRACK
		sched_ravg_window_change(*pval);
#endif
	}

	return count;
}

static int hmbird_common_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", *(int*) m->private);
	return 0;
}

static int hmbird_common_open(struct inode *inode, struct file *file)
{
	return single_open(file, hmbird_common_show, pde_data(inode));
}

HMBIRD_PROC_OPS(common, hmbird_common_open, hmbird_common_write);

/* yield_opt ops begin */
static int yield_opt_show(struct seq_file *m, void *v)
{
	struct yield_opt_params *data = m->private;

	seq_printf(m, "yield_opt:{\"enable\":%d; \"frame_per_sec\":%d; \"headroom\":%d}\n",
				data->enable, data->frame_per_sec, data->yield_headroom);
	return 0;
}

static int yield_opt_open(struct inode *inode, struct file *file)
{
	return single_open(file, yield_opt_show, pde_data(inode));
}

static ssize_t yield_opt_write(struct file *file, const char __user *buf,
							size_t count, loff_t *ppos)
{
	char *data;
	int enable_tmp, frame_per_sec_tmp, yield_headroom_tmp, cpu;
	unsigned long flags;

	data = kmalloc(count + 1, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	if(copy_from_user(data, buf, count)) {
		kfree(data);
		return -EFAULT;
	}

	data[count] = '\0';

	if (sscanf(data, "%d %d %d", &enable_tmp, &frame_per_sec_tmp, &yield_headroom_tmp) != 3) {
		kfree(data);
		return -EINVAL;
	}

	if ((enable_tmp != 0 && enable_tmp != 1) ||
		(frame_per_sec_tmp != 30 && frame_per_sec_tmp != 60 && frame_per_sec_tmp != 90 && frame_per_sec_tmp != 120) ||
		(yield_headroom_tmp < 1 || yield_headroom_tmp > 20)) {
		kfree(data);
		return -EINVAL;
	}

	yield_opt_params.frame_time_ns = NSEC_PER_SEC / frame_per_sec_tmp;
	yield_opt_params.frame_per_sec = frame_per_sec_tmp;
	yield_opt_params.yield_headroom = yield_headroom_tmp;
	yield_opt_params.enable = enable_tmp;

	for_each_possible_cpu(cpu) {
		struct sched_yield_state *ys = &per_cpu(ystate, cpu);
		raw_spin_lock_irqsave(&ys->lock, flags);
		ys->last_yield_time = 0;
		ys->last_update_time = 0;
		ys->sleep_end = 0;
		ys->yield_cnt = 0;
		ys->yield_cnt_after_sleep = 0;
		ys->sleep = 0;
		ys->sleep_times = 0;
		raw_spin_unlock_irqrestore(&ys->lock, flags);
	}

	kfree(data);
	return count;
}

HMBIRD_PROC_OPS(yield_opt, yield_opt_open, yield_opt_write);
#ifdef CONFIG_ARCH_MEDIATEK
/* boost_policy ops begin */
static int boost_policy_show(struct seq_file *m, void *v)
{
	struct boost_policy_params *data = m->private;

	seq_printf(m, "boost_policy:{\"enable\":%d; \"bottom_freq\":%u; \"boost_weight\":%d}\n",
				data->enable, data->bottom_freq, data->boost_weight);
	return 0;
}

static int boost_policy_open(struct inode *inode, struct file *file)
{
	return single_open(file, boost_policy_show, pde_data(inode));
}

static ssize_t boost_policy_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	char *data;
	int enable_tmp, bottom_freq_tmp, boost_weight_tmp;

	if (count > 64)
		return  -EFAULT;

	data = kmalloc(count + 1, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	if(copy_from_user(data, buf, count)) {
		kfree(data);
		return -EFAULT;
	}

	data[count] = '\0';

	if (sscanf(data, "%d %d %d", &enable_tmp, &bottom_freq_tmp, &boost_weight_tmp) != 3) {
		kfree(data);
		return -EINVAL;
	}

	if ((enable_tmp != 0 && enable_tmp != 1) ||
		(boost_weight_tmp < 50 || boost_weight_tmp > 300) ||
		(bottom_freq_tmp < 400000 || bottom_freq_tmp > 2200000)) {
		kfree(data);
		return -EINVAL;
	}

	boost_policy_params.bottom_freq = bottom_freq_tmp;
	boost_policy_params.boost_weight = boost_weight_tmp;
	boost_policy_params.enable = enable_tmp;

	kfree(data);
	return count;
}

HMBIRD_PROC_OPS(boost_policy, boost_policy_open, boost_policy_write);

/* tick_hit ops begin */
static int tick_hit_show(struct seq_file *m, void *v)
{
	struct tick_hit_params *data = m->private;

	seq_printf(m, "tick_hit:{\"enable\":%d; \"hit_count_thres\":%d; \"jiffies_num\":%lu}\n",
				data->enable, data->hit_count_thres, data->jiffies_num);
	return 0;
}

static int tick_hit_open(struct inode *inode, struct file *file)
{
	return single_open(file, tick_hit_show, pde_data(inode));
}

static ssize_t tick_hit_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	char *data;
	int enable_tmp, hit_count_thres_tmp, jiffies_num_tmp;

	if (count > 64)
		return  -EFAULT;

	data = kmalloc(count + 1, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	if(copy_from_user(data, buf, count)) {
		kfree(data);
		return -EFAULT;
	}

	data[count] = '\0';

	if (sscanf(data, "%d %d %d", &enable_tmp, &hit_count_thres_tmp, &jiffies_num_tmp) != 3) {
		kfree(data);
		return -EINVAL;
	}

	if ((enable_tmp != 0 && enable_tmp != 1)) {
		kfree(data);
		return -EINVAL;
	}

	tick_hit_params.hit_count_thres = hit_count_thres_tmp;
	tick_hit_params.jiffies_num = jiffies_num_tmp;
	tick_hit_params.enable = enable_tmp;

	kfree(data);
	return count;
}

HMBIRD_PROC_OPS(tick_hit, tick_hit_open, tick_hit_write);
#endif

extern struct proc_dir_entry *hmbird_dir;
struct proc_dir_entry *load_track_dir;
struct proc_dir_entry *freq_gov_dir;

static void create_procs(void)
{
	int i;

	if (hmbird_dir) {
		for (i = 0; i < ARRAY_SIZE(files_name); i++) {
			proc_create_data(files_name[i],
					 S_IRUGO | S_IWUGO,
					 hmbird_dir,
					 &common_proc_ops,
					 file_data[i]);
		}
		load_track_dir = proc_mkdir(LOAD_TRACK_DIR, hmbird_dir);
		if (load_track_dir) {
			for (i = 0; i < ARRAY_SIZE(slim_walt_config); i++) {
				proc_create_data(slim_walt_config[i],
						 S_IRUGO | S_IWUGO,
						 load_track_dir,
						 &common_proc_ops,
						 slim_walt_data[i]);
			}
		}
		freq_gov_dir = proc_mkdir(SLIM_FREQ_GOV_DIR, hmbird_dir);
		if (freq_gov_dir) {
			for (i = 0; i < ARRAY_SIZE(slim_freq_gov_config); i++) {
				proc_create_data(slim_freq_gov_config[i],
						 S_IRUGO | S_IWUGO,
						 freq_gov_dir,
						 &common_proc_ops,
						 slim_freq_gov_data[i]);
			}
		}
		proc_create_data("yield_opt",
				S_IRUGO | S_IWUGO,
				hmbird_dir,
				&yield_opt_proc_ops,
				&yield_opt_params);
#ifdef CONFIG_ARCH_MEDIATEK
		proc_create_data("boost_policy",
				S_IRUGO | S_IWUGO,
				hmbird_dir,
				&boost_policy_proc_ops,
				&boost_policy_params);
		proc_create_data("tick_hit",
				S_IRUGO | S_IWUGO,
				hmbird_dir,
				&tick_hit_proc_ops,
				&tick_hit_params);
#endif
	}
}


void hmbird_sysctrl_init(void)
{
	create_procs();
}

