/* SPDX-License-Identifier: GPL-2.0-only */
/* Included by the lifecycle owner. No MAC, IRQ, timer or DMA producer runs in
 * this mode. One fixed test per fresh PHY lifetime; no arbitrary CSR interface.
 * A userspace death cannot extend the synchronous five-second target window (plus in-flight I2C/disable latency).
 */
module_param(isolated_tx_bench, bool, 0400);
MODULE_PARM_DESC(isolated_tx_bench, "Disconnected activation bench: disable normal PHY start and permit one finite TX experiment");

static const u32 qiso_regs[] = {
 EN7581_XGPON_PHY_XG_CONTINUE_CTRL,
 EN7581_XGPON_PHY_XG_CONTINUE_CFG_PATTERN_LOWER,
 EN7581_XGPON_PHY_XG_CONTINUE_CFG_PATTERN_UPPER,
 EN7581_XPON_PMA_XPON_SETTING_0,
 EN7581_XPON_PMA_XPON_SETTING_1,
 EN7581_XGPON_PHY_TX_BURST_ADJUST,
 EN7581_XPON_PMA_TX_DLY_CTRL,
 EN7581_XPON_PMA_RG_EXT_BEN_DATA,
 EN7581_XPON_PMA_RG_PRE_BEN_DATA,
 EN7581_XPON_PMA_BENOFF_CTRL,
 EN7581_XPON_PMA_rg_force_da_pxp_txpll_ckout_en,
 EN7581_XPON_PMA_SS_LCPLL_PWCTL_SETTING_0,
 EN7581_XPON_PMA_ADD_LCPLL_RO_1,
 EN7581_XPON_PMA_RO_TDC_TX_FREQDET,
 EN7581_XPON_ANA_RG_PXP_TXPLL_PHY_CK1_EN,
 EN7581_XGPON_PHY_SFP_STA,
};
static struct {
 unsigned int id, valid, dark_checks;
 int error, restore_error;
 bool used, restored, tx_off;
 u64 enabled_ns, disabled_ns, window_ns;
 u32 words[3][ARRAY_SIZE(qiso_regs)];
} qiso;
static struct {
 struct en7573_mpd sample[3];
 u64 begin[3], end[3];
 int error[3];
 unsigned int count;
} qiso_mpd;

static int qiso_dark(void)
{
 u32 sfp;
 int ret = q1000k_pon_get_los(qphy_controller);
 if (ret < 0) return ret;
 if (!ret) return -ENOLINK;
 ret = an7581_pon_phy_read(EN7581_XGPON_PHY_SFP_STA, &sfp);
 if (ret || sfp == ~0U) return ret ?: -EIO;
 if (!(sfp & EN7581_XGPON_PHY_SFP_RX_LOS_ST)) return -ENOLINK;
 qiso.dark_checks++;
 return 0;
}
static int qiso_sample(unsigned int phase)
{
 unsigned int i;
 int ret;
 for (i = 0; i < ARRAY_SIZE(qiso_regs); i++) {
  ret = an7581_pon_phy_read(qiso_regs[i], &qiso.words[phase][i]);
  if (ret) return ret;
  /* Pattern data and full-width frequency values can be all ones. */
  if (i != 1 && i != 2 && i != 13 && qiso.words[phase][i] == ~0U) return -EIO;
 }
 qiso.valid |= BIT(phase);
 return 0;
}

static int qiso_probe(unsigned int phase, bool active)
{
 int ret = qiso_dark();
 if (ret) return ret;
 qiso_mpd.begin[phase] = ktime_get_boottime_ns();
 ret = q1000k_pon_measure_mpd(qphy_controller, active, &qiso_mpd.sample[phase]);
 qiso_mpd.end[phase] = ktime_get_boottime_ns();
 qiso_mpd.error[phase] = ret;
 qiso_mpd.count = phase + 1;
 return ret ?: qiso_dark();
}

#include "q1000k_phy_output.h"

static int qiso_run(unsigned int id)
{
 struct en7573_tx_recipe saved = { 0 };
 bool enabled = true;
 /* 19 passive reference, then paired OEM MPD measurements. */
 static const unsigned int options[] = { 3, 17, 3, 14, 9, 10, 12, 15, 7, 6, 8, 2, 3 };
 unsigned int option, tick, recipe = 0, i;
 bool measurement = id >= 19;
 u32 pattern = 0, data = 0, control = 0;
 u64 window_start = 0;
 int ret, restore = 0, next;
 if (id >= 32) return qout_run(id);
 if (!isolated_tx_bench) return -EPERM;
 if (id < 1 || id > 31) return -EINVAL;
 option = measurement ? options[id - 19] : id;
 if (qiso.used) return -EALREADY;
 if (qphy_context()) return -EWOULDBLOCK;
 /* Acquisition/configuration uses the same checked sleepable lifecycle as
  * normal startup, with optical TX disabled and all interrupts masked. */
 ret = q1000k_phy_prepare_wan();
 if (!ret) ret = q1000k_phy_configure(PHY_XGSPON_CONFIG);
 if (ret) return ret;
 mutex_lock(&qphy_control);
 qphy_callback_lock();
 ret = qphy_ready();
 if (!ret && (qphy_active || qphy_irq_dev || !qphy_controller)) ret = -EBUSY;
 if (!ret) ret = q1000k_pon_get_tx(qphy_controller, &enabled);
 if (!ret && enabled) ret = -EBUSY;
 /* Empty restore checks activation-DT authorization in the controller. */
 if (!ret) ret = q1000k_pon_tx_recipe(qphy_controller, 0, &saved, true);
 if (!ret) ret = qiso_dark();
 if (ret) goto out;
 qiso.used = true; qiso.id = id;
 ret = qiso_sample(0);
 if (ret) goto finish;
 /* Refuse an already enabled generator; cleanup must return to normal. */
 if (qiso.words[0][0] & 1) { ret = -EBUSY; goto finish; }
 if (option >= 3) {
  /* Native AN7581 no-downstream PRBS preparation, also used in the vendor
   * PHY: FIRST_PLUG_IN -> PLUG_OUT -> 350ms. No fake RX lock or IRQ. */
  fiber_plug_reset(FIRST_PLUG_IN, gpPhyPriv->wan_sel);
  ret = q1000k_phy_controller_check() ?: an7581_pon_phy_status();
  if (!ret) {
   fiber_plug_reset(PLUG_OUT, gpPhyPriv->wan_sel);
   ret = q1000k_phy_controller_check() ?: an7581_pon_phy_status();
  }
  if (ret) goto finish;
  msleep(350);
 }
 if (option >= 10 && option <= 13) recipe = option - 8; /* OEM0/1, Sir0/1 */
 if (option == 14) recipe = 6; /* BEN forced-off negative control */
 if (option == 15) recipe = 1; /* loop restart alone */
 if (recipe) {
  ret = q1000k_pon_tx_recipe(qphy_controller, recipe, &saved, false);
  if (ret) goto finish;
 }
 if (option == 9) {
  ret = qphy_reg_write(qiso_regs[3], qiso.words[0][3] ^ BIT(8));
  if (ret) goto finish;
 }
 if (option == 4) pattern = 1; /* PRBS23 */
 if (option == 5) pattern = 2; /* PRBS31 */
 if (option >= 6 && option <= 8) {
  pattern = 3;
  data = option == 7 ? ~0U : option == 8 ? 0xaaaaaaaa : 0;
  ret = qphy_reg_write(qiso_regs[1], data);
  if (!ret) ret = qphy_reg_write(qiso_regs[2], data);
  if (ret) goto finish;
 }
 /* #1: controller enable with normal MAC gating and no packet producer.
  * #16: native in-timeslot mode (no OLT grants). #17: generator/TX-off.
  */
 if (option != 1) control = 1 | (pattern << 8) | (option == 16 ? 0 : BIT(16));
 ret = qphy_reg_write(qiso_regs[0], control);
 if (!ret) ret = qiso_dark();
 if (!ret && measurement) ret = qiso_probe(0, id != 19);
 if (!ret && option != 17) {
  ret = q1000k_pon_set_tx(qphy_controller, true);
  if (!ret) qiso.enabled_ns = ktime_get_boottime_ns();
 }
 window_start = qiso.enabled_ns ?: ktime_get_boottime_ns();
 qiso.window_ns = window_start;
 if (!ret) ret = qiso_sample(1);
 for (tick = 0; !ret && tick < 20; tick++) {
  if (measurement && tick == 4) {
   ret = qiso_probe(1, id != 19);
   if (ret) break;
  }
  u64 elapsed = ktime_get_boottime_ns() - window_start;
  unsigned int remaining;
  if (elapsed >= 5000000000ULL) break;
  remaining = (5000000000ULL - elapsed) / 1000000;
  if (!remaining) break;
  msleep(remaining < 250 ? remaining : 250);
  ret = qiso_dark();
 }
finish:
 /* Always disable the independent laser gate before restoring pattern or
  * calibration. Even a failed test leaves an explicit cleanup result. */
 restore = q1000k_pon_set_tx(qphy_controller, false);
 qiso.disabled_ns = ktime_get_boottime_ns();
 if (!restore) {
  next = q1000k_pon_get_tx(qphy_controller, &enabled);
  if (!next && !enabled) qiso.tx_off = true;
  else restore = next ?: -EIO;
 }
 if (qiso.valid & 1) {
  next = qphy_reg_write(qiso_regs[0], 0);
  if (next && !restore) restore = next;
  /* Restore only the four words this experiment directly changes. PMA
   * initialization is discarded with this one-shot PHY lifetime. */
  for (i = 3; i; i--) {
   next = qphy_reg_write(qiso_regs[i], qiso.words[0][i]);
   if (next && !restore) restore = next;
  }
  next = qphy_reg_write(qiso_regs[0], qiso.words[0][0]);
  if (next && !restore) restore = next;
 }
 if (saved.count) {
  next = q1000k_pon_tx_recipe(qphy_controller, recipe, &saved, true);
  if (next && !restore) restore = next;
 }
 next = qiso_sample(2);
 if (next && !restore) restore = next;
 if (measurement && !ret && !restore) {
  next = qiso_probe(2, id != 19);
  if (next) ret = next;
 }
 qiso.restore_error = restore;
 qiso.restored = !restore && !saved.count && (qiso.valid & 1);
 if (restore && !ret) ret = restore;
 if (ret && ret != -ENODATA) qphy_failed(ret);
out:
 qiso.error = ret;
 qphy_callback_unlock();
 mutex_unlock(&qphy_control);
 return ret;
}
static int qiso_set(const char *value, const struct kernel_param *kp)
{
 unsigned int id;
 int ret = kstrtouint(value, 10, &id);
 return ret ?: qiso_run(id);
}
static int qiso_get(char *buffer, const struct kernel_param *kp)
{
 unsigned int i, p;
 int len;
 mutex_lock(&qphy_control);
 len = scnprintf(buffer, PAGE_SIZE,
  "{\"isolated_tx_version\":1,\"id\":%u,\"error\":%d,\"restore_error\":%d,"
  "\"restored\":%s,\"tx_off\":%s,\"valid_phases\":%u,\"dark_checks\":%u,"
  "\"enabled_ns\":%llu,\"disabled_ns\":%llu,\"window_ns\":%llu,\"registers\":[",
  qiso.id, qiso.error, qiso.restore_error, qiso.restored ? "true" : "false",
  qiso.tx_off ? "true" : "false", qiso.valid, qiso.dark_checks,
  (unsigned long long)qiso.enabled_ns, (unsigned long long)qiso.disabled_ns,
  (unsigned long long)qiso.window_ns);
 for (i = 0; i < ARRAY_SIZE(qiso_regs); i++)
  len += scnprintf(buffer+len, PAGE_SIZE-len, "%s%u", i ? "," : "", qiso_regs[i]);
 len += scnprintf(buffer+len, PAGE_SIZE-len, "],\"samples\":[");
 for (p = 0; p < 3; p++) {
  len += scnprintf(buffer+len, PAGE_SIZE-len, "%s[", p ? "," : "");
  for (i = 0; i < ARRAY_SIZE(qiso_regs); i++)
   len += scnprintf(buffer+len, PAGE_SIZE-len, "%s%u", i ? "," : "", qiso.words[p][i]);
  len += scnprintf(buffer+len, PAGE_SIZE-len, "]");
 }
 len += scnprintf(buffer+len, PAGE_SIZE-len, "]}\n");
 mutex_unlock(&qphy_control);
 return len;
}
static const struct kernel_param_ops qiso_ops = { .set = qiso_set, .get = qiso_get };
module_param_cb(isolated_tx_test, &qiso_ops, NULL, 0600);

/* Cached evidence only: reading this parameter never runs a probe. */
static int qiso_mpd_get(char *buffer, const struct kernel_param *kp)
{
 unsigned int p, r, i;
 int len;
 mutex_lock(&qphy_control);
 len = scnprintf(buffer, PAGE_SIZE,
  "{\"mpd_version\":1,\"id\":%u,\"conversion_ready_verified\":false,"
  "\"registers\":[828,240,102,100,106,964,968,992,1160],\"probes\":[", qiso.id);
 for (p = 0; p < qiso_mpd.count; p++) {
  struct en7573_mpd *v = &qiso_mpd.sample[p];
  len += scnprintf(buffer+len, PAGE_SIZE-len,
   "%s{\"phase\":%u,\"begin_ns\":%llu,\"end_ns\":%llu,\"active\":%s,\"error\":%d,"
   "\"restored\":%s,\"restore_error\":%d,\"selected_valid\":%u,\"saved\":[%u,%u,%u],"
   "\"selected\":[%u,%u,%u],\"valid\":[%u,%u,%u],\"sample_error\":[%d,%d,%d],\"values\":[",
   p ? "," : "", p, (unsigned long long)qiso_mpd.begin[p], (unsigned long long)qiso_mpd.end[p],
   v->active ? "true" : "false", qiso_mpd.error[p], v->restored ? "true" : "false", v->restore_error,
   v->selected_valid, v->saved[0], v->saved[1], v->saved[2], v->selected[0], v->selected[1], v->selected[2],
   v->valid[0], v->valid[1], v->valid[2], v->sample_error[0], v->sample_error[1], v->sample_error[2]);
  for (r = 0; r < 3; r++) {
   len += scnprintf(buffer+len, PAGE_SIZE-len, "%s[", r ? "," : "");
   for (i = 0; i < EN7573_MPD_FIELDS; i++)
    len += scnprintf(buffer+len, PAGE_SIZE-len, "%s%u", i ? "," : "", v->values[r][i]);
   len += scnprintf(buffer+len, PAGE_SIZE-len, "]");
  }
  len += scnprintf(buffer+len, PAGE_SIZE-len, "]}");
 }
 len += scnprintf(buffer+len, PAGE_SIZE-len, "]}\n");
 mutex_unlock(&qphy_control);
 return len;
}
static const struct kernel_param_ops qiso_mpd_ops = { .get = qiso_mpd_get };
module_param_cb(isolated_mpd, &qiso_mpd_ops, NULL, 0400);
