/* SPDX-License-Identifier: GPL-2.0-only */
/* Fixed disconnected-output experiments; included by the isolated owner. */
#define QOUT_SAMPLES 5
static struct {
 struct en7573_output_sample sample[3][QOUT_SAMPLES];
 u64 begin[3][QOUT_SAMPLES], end[3][QOUT_SAMPLES];
 unsigned int count[3], gates;
 bool fixed_monitor;
} qout;

static int qout_capture(unsigned int phase)
{
 unsigned int row = qout.count[phase];
 int ret = qiso_dark();
 if (ret || row >= QOUT_SAMPLES) return ret ?: -EOVERFLOW;
 qout.begin[phase][row] = ktime_get_boottime_ns();
 ret = q1000k_pon_output_sample(qphy_controller, &qout.sample[phase][row]);
 qout.end[phase][row] = ktime_get_boottime_ns();
 qout.count[phase]++;
 return ret ?: qiso_dark();
}

static int qout_observe(unsigned int phase, u64 start)
{
 static const unsigned int off_ms[] = { 0, 100, 300, 600, 1000 };
 static const unsigned int on_ms[] = { 0, 100, 500, 1500, 4500 };
 unsigned int row;
 int ret;
 for (row = 0; row < QOUT_SAMPLES; row++) {
  u64 target = start + (u64)(phase == 1 ? on_ms[row] : off_ms[row]) * 1000000;
  u64 now;
  while ((now = ktime_get_boottime_ns()) < target) {
   u64 left = (target - now) / 1000000;
   msleep(left > 100 ? 100 : left ? left : 1);
   ret = qiso_dark();
   if (ret) return ret;
  }
  if (phase == 1 && ktime_get_boottime_ns() - start >= 5000000000ULL)
   return -ETIMEDOUT;
  ret = qout_capture(phase);
  if (ret) return ret;
 }
 return 0;
}

static int qout_run(unsigned int id)
{
 struct en7573_tx_recipe eye = { 0 };
 struct en7573_output_hold hold = { 0 };
 unsigned int option = 3, recipe = 0, i;
 bool enabled = true;
 u64 start;
 int ret, cleanup = 0, next;
 if (!isolated_tx_bench) return -EPERM;
 if (id < 32 || id > 48) return -EINVAL;
 if (qiso.used) return -EALREADY;
 if (qphy_context()) return -EWOULDBLOCK;
 ret = q1000k_phy_prepare_wan();
 if (!ret) ret = q1000k_phy_configure(PHY_XGSPON_CONFIG);
 if (ret) return ret;
 mutex_lock(&qphy_control);
 qphy_callback_lock();
 ret = qphy_ready();
 if (!ret && (qphy_active || qphy_irq_dev || !qphy_controller)) ret = -EBUSY;
 if (!ret) ret = q1000k_pon_get_tx(qphy_controller, &enabled);
 if (!ret && enabled) ret = -EBUSY;
 if (!ret) ret = q1000k_pon_tx_recipe(qphy_controller, 0, &eye, true);
 if (!ret) ret = qiso_dark();
 if (ret) goto out;
 qiso.used = true; qiso.id = id;
 qout.fixed_monitor = id >= 35;
 qout.gates = id == 32 || id == 36 ? 1 : id == 34 || id == 37 ? 2 : id == 48 ? 0 : 3;
 ret = q1000k_pon_output_gates(qphy_controller, 0);
 if (!ret) ret = qiso_sample(0);
 if (ret) goto finish;
 if (qiso.words[0][0] & 1) { ret = -EBUSY; goto finish; }
 if (id == 40) option = 1;
 if (id == 41) option = 9;
 if (id == 42) option = 2;
 if (id == 45) option = 7;
 if (id == 46) option = 6;
 if (option >= 3) {
  fiber_plug_reset(FIRST_PLUG_IN, gpPhyPriv->wan_sel);
  ret = q1000k_phy_controller_check() ?: an7581_pon_phy_status();
  if (!ret) {
   fiber_plug_reset(PLUG_OUT, gpPhyPriv->wan_sel);
   ret = q1000k_phy_controller_check() ?: an7581_pon_phy_status();
  }
  if (ret) goto finish;
  msleep(350);
 }
 if (id == 38) recipe = 2;
 if (id == 39) recipe = 4;
 if (id == 43) recipe = 6;
 if (id == 44) recipe = 1;
 if (recipe) ret = q1000k_pon_tx_recipe(qphy_controller, recipe, &eye, false);
 if (!ret && option == 9) ret = qphy_reg_write(qiso_regs[3], qiso.words[0][3] ^ BIT(8));
 if (!ret && (option == 6 || option == 7)) {
  ret = qphy_reg_write(qiso_regs[1], option == 7 ? ~0U : 0);
  if (!ret) ret = qphy_reg_write(qiso_regs[2], option == 7 ? ~0U : 0);
 }
 if (!ret) ret = qphy_reg_write(qiso_regs[0], option == 1 ? 0 :
     1 | BIT(16) | ((option == 6 || option == 7) ? 3U << 8 : 0));
 if (!ret && qout.fixed_monitor) ret = q1000k_pon_output_hold(qphy_controller, &hold, false);
 if (!ret) ret = qout_observe(0, ktime_get_boottime_ns());
 if (!ret) ret = qiso_dark();
 if (!ret) ret = q1000k_pon_output_gates(qphy_controller, qout.gates);
 start = ktime_get_boottime_ns();
 qiso.window_ns = start;
 if (!ret && (qout.gates & 1)) qiso.enabled_ns = start;
 if (!ret) ret = qiso_sample(1);
 if (!ret) ret = qout_observe(1, start);
 while (!ret) {
  u64 elapsed = ktime_get_boottime_ns() - start;
  u64 left;
  if (elapsed >= 5000000000ULL) break;
  left = (5000000000ULL - elapsed) / 1000000;
  msleep(left > 100 ? 100 : left ? left : 1);
  ret = qiso_dark();
 }
finish:
 /* Both gates off before the final measurements. Keep pattern, eye, monitor
  * gain/mux and loop settings unchanged until all off samples are recorded.
  */
 cleanup = q1000k_pon_output_gates(qphy_controller, 0);
 qiso.disabled_ns = ktime_get_boottime_ns();
 if (!cleanup) {
  next = q1000k_pon_get_tx(qphy_controller, &enabled);
  if (!next && !enabled) qiso.tx_off = true;
  else cleanup = next ?: -EIO;
 }
 if (!ret && !cleanup) ret = qout_observe(2, qiso.disabled_ns);
 if (hold.saved_valid) {
  next = q1000k_pon_output_hold(qphy_controller, &hold, true);
  if (next && !cleanup) cleanup = next;
 }
 if (eye.count) {
  next = q1000k_pon_tx_recipe(qphy_controller, recipe, &eye, true);
  if (next && !cleanup) cleanup = next;
 }
 if (qiso.valid & 1) {
  next = qphy_reg_write(qiso_regs[0], 0);
  if (next && !cleanup) cleanup = next;
  for (i = 3; i; i--) {
   next = qphy_reg_write(qiso_regs[i], qiso.words[0][i]);
   if (next && !cleanup) cleanup = next;
  }
  next = qphy_reg_write(qiso_regs[0], qiso.words[0][0]);
  if (next && !cleanup) cleanup = next;
 }
 next = qiso_sample(2);
 if (next && !cleanup) cleanup = next;
 qiso.restore_error = cleanup;
 qiso.restored = !cleanup && !hold.saved_valid && !eye.count && (qiso.valid & 1);
 if (!ret) ret = cleanup;
 if (ret && ret != -ENODATA) qphy_failed(ret);
out:
 qiso.error = ret;
 qphy_callback_unlock();
 mutex_unlock(&qphy_control);
 return ret;
}

static int qout_get(char *buffer, unsigned int phase)
{
 unsigned int r, i;
 int len;
 mutex_lock(&qphy_control);
 len = scnprintf(buffer, PAGE_SIZE,
  "{\"output_version\":1,\"id\":%u,\"phase\":%u,\"gates\":%u,\"fixed_monitor\":%s,"
  "\"conversion_ready_verified\":false,\"samples\":[",
  qiso.id, phase, qout.gates, qout.fixed_monitor ? "true" : "false");
 for (r = 0; r < qout.count[phase]; r++) {
  struct en7573_output_sample *s = &qout.sample[phase][r];
  len += scnprintf(buffer + len, PAGE_SIZE - len,
   "%s{\"begin_ns\":%llu,\"end_ns\":%llu,\"valid\":%u,\"error\":%d,\"board_disabled\":%d,\"v\":[",
   r ? "," : "", (unsigned long long)qout.begin[phase][r],
   (unsigned long long)qout.end[phase][r], s->valid, s->error, s->board_disabled);
  for (i = 0; i < EN7573_OUTPUT_FIELDS; i++)
   len += scnprintf(buffer + len, PAGE_SIZE - len, "%s%u", i ? "," : "", s->values[i]);
  len += scnprintf(buffer + len, PAGE_SIZE - len, "]}");
 }
 len += scnprintf(buffer + len, PAGE_SIZE - len, "]}\n");
 mutex_unlock(&qphy_control);
 return len;
}
#define QOUT_PARAM(name, phase) \
 static int name##_get(char *buf, const struct kernel_param *kp) { return qout_get(buf, phase); } \
 static const struct kernel_param_ops name##_ops = { .get = name##_get }; \
 module_param_cb(name, &name##_ops, NULL, 0400)
QOUT_PARAM(output_before, 0);
QOUT_PARAM(output_on, 1);
QOUT_PARAM(output_after, 2);
#undef QOUT_PARAM
