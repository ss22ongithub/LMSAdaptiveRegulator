/**
 * Dynamic adaptive memory bandwidth controller for multi-core systems
 *
 *
 * This file is distributed under GPL v2 License. 
 * See LICENSE.TXT for details.
 *
 */

/**************************************************************************
 * Included Files
 **************************************************************************/
#include "kernel_headers.h"
#include "ar.h"
#include "ar_debugfs.h"


/**************************************************************************
 * Constants /Macros
 **************************************************************************/
#define BUF_SIZE 256

/**************************************************************************
 * Globals
 **************************************************************************/
static u32 ar_regulation_time_ms = 1; //ms, default 1000ms
static u32 ar_observation_time_ms = 1000;
atomic_t enable_reg; // Memory regulation enabled or disabled via debugfs
static struct dentry *ar_dir = NULL;

/****************************************
 * Fops functions for Regulation interval
 ****************************************/
static int ar_reg_interval_show(struct seq_file *m, void *v)
{
    int tmp = ar_regulation_time_ms;
    pr_info("%s: Reading.",__func__);
    seq_printf(m, "%u\n",tmp);
    return 0;
}

static int ar_reg_interval_open(struct inode *inode, struct file *filp)
{
    return single_open(filp, ar_reg_interval_show, NULL);
}

static ssize_t ar_reg_interval_write(struct file *filp,
                    const char __user *ubuf,
                    size_t cnt, loff_t *ppos){

    char buf[BUF_SIZE];
    u32 tmp = 0 ;

    if (copy_from_user(&buf, ubuf, (cnt > BUF_SIZE) ? BUF_SIZE: cnt) != 0)
        return 0;

    pr_info("%s: Received %s",__func__,buf);

    int ret = kstrtou32(buf, 10, &tmp);

    if (ret){
        pr_err("%s: ret %d",__func__,ret);
        return ret;
    }

    /* Used as a divisor in the bandwidth conversions */
    if (tmp == 0)
        return -EINVAL;

    ar_regulation_time_ms =  tmp;
    return cnt;
}

/****************************************
 * Fops functions for Observation interval
 ****************************************/
static ssize_t ar_obs_interval_write(struct file *filp,
                    const char __user *ubuf,
                    size_t cnt, loff_t *ppos){

    char buf[BUF_SIZE];
    u32 tmp = 0 ;

    if (copy_from_user(&buf, ubuf, (cnt > BUF_SIZE) ? BUF_SIZE: cnt) != 0)
        return 0;

    pr_info("%s: Received %s",__func__,buf);

    int ret = kstrtou32(buf, 10, &tmp);

    if (ret){
        pr_err("%s: ret %d",__func__,ret);
        return ret;
    }

    ar_observation_time_ms =  tmp;
    return cnt;
}

static int ar_obs_interval_show(struct seq_file *m, void *v)
{
    int tmp = ar_observation_time_ms;
    pr_info("%s: Reading.",__func__);
    seq_printf(m, "%u \n",tmp);
    return 0;
}


static int ar_obs_interval_open(struct inode *inode, struct file *filp)
{
    return single_open(filp, ar_obs_interval_show, NULL);
}

/******************************************************
 Fops functions for enable/disable regulation interval
******************************************************/
static ssize_t ar_enable_reg_write(struct file *filp,
                                const char __user *ubuf,size_t cnt, loff_t *ppos) {
    char buf[BUF_SIZE];
    memset(buf,0,sizeof(buf));
    u8 user_value = false ;

    if (copy_from_user(&buf, ubuf, (cnt > BUF_SIZE) ? BUF_SIZE: cnt) != 0)
        return 0;

    pr_info("%s: Received %s",__func__,buf);

    int ret = kstrtou8(buf, 10, &user_value);

    if (ret || (user_value > 1) ){
        pr_err("%s: Failed to update: Wrong value %u (error:%d)",__func__,user_value,ret);
        return -EINVAL;
    }

    if ( atomic_read(&enable_reg) == user_value ){
        pr_info("Regulation %s",(user_value?"Enabled":"Disabled"));
        return cnt;
    }

    atomic_set(&enable_reg,(user_value?true:false) );

    /* Use new wrapper functions that coordinate master thread and per-core regulation */
    if (user_value){
        pr_info("%s: Enabling regulation for all cores",__func__);
        start_all_regulation();
    } else {
        pr_info("%s: Disabling regulation for all cores",__func__);
        stop_all_regulation();
    }

    pr_info("Regulation %s",(user_value?"Enabled":"Disabled"));
    return cnt;
}

static int ar_enable_reg_read(struct seq_file *m, void *v)
{
    int tmp = atomic_read(&enable_reg);
    seq_printf(m, "%u \n",tmp);
    return 0;
}

static int ar_enable_reg_open(struct inode *inode, struct file *filp)
{
    return single_open(filp,ar_enable_reg_read , NULL);
}
/******************************************************
 * ml_stats: live view of the ML predictor, per core
 *
 * pred_*   latest prediction: minimum per-ms bandwidth over the NEXT 10 ms
 * real_*   what the most recently scored prediction turned out to be
 * mae_*    running mean absolute error of the predictions
 * over_*   % of scored predictions above the realised floor (QoS-risk side)
 * est_*    budget estimate the loop arms the counters with (after scale,
 *          headroom and clamp; the proportional cap is applied on top)
 * infer_us / infer_max_us  time of the last / slowest ml_predict()
 *          (= one FPU section: feature build + both forests)
 * resets   history restarts (sample gap > 2 ms or a counter not running)
 * ipi_fail smp_call_function_single() failures (core offline)
 * Bandwidth = per-core calc bandwidth (cache-misses / LLC-store-misses x
 * 64 B), in MiB/s, the unit the regulator uses throughout.
 * This file only reads integers: no FPU use.
 ******************************************************/
static int ar_ml_stats_show(struct seq_file *m, void *v)
{
    u8 cpu_id;

    seq_printf(m, "# ml_enable=%d selftest=%s counters=%s\n", ml_enable ? 1 : 0,
               ml_selftest_state > 0 ? "passed" : (ml_selftest_state < 0 ? "FAILED" : "not-run"),
               ar_ml_counters_available() ? "ok" : "MISSING");
    seq_puts(m, "cpu ready n_pred n_eval pred_rd pred_wr real_rd real_wr "
                "mae_rd mae_wr over_rd% over_wr% est_rd est_wr infer_us infer_max_us "
                "fpu_skip resets ipi_fail\n");
    for (cpu_id = 1; cpu_id <= g_num_regulated_cpus; cpu_id++) {
        struct core_info *c = get_core_info(cpu_id);

        if (!c)
            continue;
        seq_printf(m, "%u %d %u %u %lld %lld %lld %lld %lld %lld %u %u %lld %lld %u %u %u %u %u\n",
                   cpu_id, ml_ready(&c->ml), c->ml.stats.n_pred, c->ml.stats.n_eval,
                   c->ml.stats.pred_rd_mbps, c->ml.stats.pred_wr_mbps,
                   c->ml.stats.real_rd_mbps, c->ml.stats.real_wr_mbps,
                   c->ml.stats.mae_rd_mbps, c->ml.stats.mae_wr_mbps,
                   c->ml.stats.over_rd_pct, c->ml.stats.over_wr_pct,
                   c->next_estimate, c->write_next_estimate,
                   c->ml_infer_ns / 1000, c->ml_infer_max_ns / 1000,
                   c->ml.stats.n_fpu_skipped, c->ml.stats.n_resets, c->ipi_failures);
    }
    return 0;
}

static int ar_ml_stats_open(struct inode *inode, struct file *filp)
{
    return single_open(filp, ar_ml_stats_show, NULL);
}

static const struct file_operations ar_ml_stats_fops = {
    .open       = ar_ml_stats_open,
    .read       = seq_read,
    .llseek     = seq_lseek,
    .release    = single_release,
};

/****************************************
 * debug Fops
 ****************************************/


static const struct file_operations ar_obs_interval_fops = {
    .open       = ar_obs_interval_open,
    .write      = ar_obs_interval_write,
    .read       = seq_read,
    .release    = single_release,
};

static const struct file_operations ar_reg_interval_fops = {
    .open       = ar_reg_interval_open,
    .write      = ar_reg_interval_write,
    .read       = seq_read,
    .release    = single_release,
};


static const struct file_operations ar_enable_reg = {
    .open       = ar_enable_reg_open,
    .write      = ar_enable_reg_write,
    .read       = seq_read,
    .release    = single_release,
};

int ar_init_debugfs(void)
{

    ar_dir = debugfs_create_dir("ar", NULL);
    BUG_ON(!ar_dir);
    debugfs_create_file("regu_interval", 0444, ar_dir, NULL,
                &ar_reg_interval_fops);
    debugfs_create_file("obs_interval", 0444, ar_dir, NULL,
                &ar_obs_interval_fops);
    debugfs_create_file("enable_regulation", 0444, ar_dir, NULL,
                        &ar_enable_reg);
    debugfs_create_file("ml_stats", 0444, ar_dir, NULL,
                        &ar_ml_stats_fops);
    return 0;
}

inline void ar_remove_debugfs(void){
    debugfs_remove_recursive(ar_dir);
}

u32  get_regulation_time(void){
	return ar_regulation_time_ms;
}

