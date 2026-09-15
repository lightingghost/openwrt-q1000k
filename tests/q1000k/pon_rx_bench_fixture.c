// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define module_param(...)
#define MODULE_PARM_DESC(...)
#define module_param_cb(...)
#define PAGE_SIZE 4096
#define scnprintf snprintf
struct kernel_param { int unused; };
struct kernel_param_ops { int (*get)(char *,const struct kernel_param *); };
struct device_node { int unused; };
static struct device_node root;
static bool board=true, bench=true, running=true, phy_mode, phy_reacquire, phy_restore_pll, phy_restore_gain, power_valid;
static int refs, preparations, phy_error, protocol_error, provider_error, samples, sample_error;
static u32 mac_mask;
static int mac_reads;
static struct device_node *of_find_node_by_path(const char *path) { assert(!strcmp(path,"/")); refs++; return &root; }
static bool of_machine_is_compatible(const char *name) { assert(!strcmp(name,"quantum,q1000k")); return board; }
static bool of_property_read_bool(struct device_node *node,const char *name) { assert(node==&root && !strcmp(name,"quantum,xgspon-bench")); return bench; }
static void of_node_put(struct device_node *node) { assert(node==&root); refs--; }
static int q1000k_protocol_status(void) { return protocol_error; }
static bool q1000k_transport_running(void) { return running; }
static u32 get_xpon_data(u32 reg) { assert(reg==0x5040); mac_reads++; return mac_mask; }
static int an7581_xpon_status(void) { return provider_error; }
/* PRODUCTION */
int q1000k_phy_set_rx_bench(bool enabled, bool reacquire, bool restore_pll, bool restore_gain) {
    preparations++; if(!phy_error) { phy_mode=enabled; phy_reacquire=reacquire; phy_restore_pll=restore_pll; phy_restore_gain=restore_gain; } return phy_error;
}
int q1000k_phy_rx_sample(struct q1000k_rx_sample *s) {
    samples++; if(sample_error) return sample_error;
    memset(s,0,sizeof(*s)); s->synced=true; s->frames=1234; s->sampled_ms=123456789012ULL;
    s->gain_restore_enabled=phy_restore_gain; s->rx_power_valid=power_valid; s->rx_power_nw=power_valid ? 19900 : 0;
    s->pll_restore_enabled=phy_restore_pll; s->reacquire_enabled=phy_reacquire; s->reacquire_attempts=phy_reacquire ? 1 : 0;
    s->receiver=(struct q1000k_rx_registers){
        .rx_control=1, .pcs_reset=2, .pma_reset=3, .clock_control=4,
        .cdr_control=5, .rx_frequency=6, .pll_status=7, .tdc_control=8,
        .rx_analog0=9, .rx_analog1=10, .rx_analog2=11,
        .rx_sequence_force=12, .rx_sequence_disable=4294967294U,
        .rx_sequence_force0=14,
        .rx_sequence_disable0=15,
        .rx_lock_force=16,
        .rx_lock_disable=17,
        .rx_oscal_control=18,
        .rx_reset0=19,
        .rx_reset1=20,
        .pll_power=21,
        .pll_filter=22,
        .pll_pcw1=23,
        .pll_pcw2=24,
        .pll_force=25,
        .pll_measure=26,
        .pll_kband=27,
        .pll_outputs=28, .rx_frontend_gain=29,

    };
    return 0;
}
int main(void) {
    char out[4096];
    assert(!q1000k_rx_bench_prepare() && !phy_mode && preparations==1);
    assert(qrx_status_get(out,NULL)==-EOPNOTSUPP && !samples);
    rx_restore_pll=true;
    assert(q1000k_rx_bench_prepare()==-EINVAL);
    rx_reacquire=true;
    assert(q1000k_rx_bench_prepare()==-EINVAL && preparations==1);
    rx_bench=true; board=false;
    assert(q1000k_rx_bench_prepare()==-EPERM && !refs && preparations==1);
    board=true; bench=false;
    assert(q1000k_rx_bench_prepare()==-EPERM && !refs && preparations==1);
    bench=true; phy_error=-EACCES;
    assert(q1000k_rx_bench_prepare()==-EACCES && !phy_mode && !refs);
    phy_error=0; assert(!q1000k_rx_bench_prepare() && phy_mode && phy_reacquire && phy_restore_pll);
    protocol_error=-EIO; assert(qrx_status_get(out,NULL)==-EIO && !samples);
    protocol_error=0; running=false; assert(qrx_status_get(out,NULL)==-EAGAIN && !samples);
    running=true; sample_error=-EAGAIN;
    assert(qrx_status_get(out,NULL)==-EAGAIN && samples==1 && !mac_reads);
    sample_error=0; mac_mask=1;
    assert(qrx_status_get(out,NULL)==-EIO && samples==2 && mac_reads==1);
    mac_mask=0; provider_error=-ENODEV;
    assert(qrx_status_get(out,NULL)==-ENODEV && samples==3 && mac_reads==2);
    provider_error=0; sample_error=-EIO;
    assert(qrx_status_get(out,NULL)==-EIO && samples==4 && mac_reads==2);
    sample_error=0; assert(qrx_status_get(out,NULL)>0 && samples==5 && mac_reads==3);
    assert(strstr(out,"\"pll_restore_enabled\":true"));
    assert(strstr(out,"\"receiver_version\":5"));
    assert(strstr(out,"\"frames\":1234") && strstr(out,"\"sampled_ms\":123456789012"));
    assert(strstr(out,"\"registration_enabled\":false") && strstr(out,"\"tx_enabled\":false"));
    assert(strstr(out,"\"reacquire_enabled\":true,\"reacquire_attempts\":1"));
    assert(strstr(out,"\"receiver\":{\"rx_control\":1,\"pcs_reset\":2,\"pma_reset\":3,"
                      "\"clock_control\":4,\"cdr_control\":5,\"rx_frequency\":6,\"pll_status\":7,"
                      "\"tdc_control\":8,\"rx_analog0\":9,\"rx_analog1\":10,\"rx_analog2\":11,"
                      "\"rx_sequence_force\":12,\"rx_sequence_disable\":4294967294,"
                      "\"rx_sequence_force0\":14,"
                      "\"rx_sequence_disable0\":15,"
                      "\"rx_lock_force\":16,"
                      "\"rx_lock_disable\":17,"
                      "\"rx_oscal_control\":18,"
                      "\"rx_reset0\":19,"
                      "\"rx_reset1\":20,"
                      "\"pll_power\":21,"
                      "\"pll_filter\":22,"
                      "\"pll_pcw1\":23,"
                      "\"pll_pcw2\":24,\"pll_force\":25,\"pll_measure\":26,\"pll_kband\":27,\"pll_outputs\":28,\"rx_frontend_gain\":29,\"sfp_status\":0,\"sfp_polarity\":0,\"digital_status\":0,\"pcs_debug_control\":0,\"serdes_control\":0,\"rx_clock_divider\":0,\"rx_bus_width\":0,\"rx_input_control\":0,\"rx_cdr_ratio\":0,\"rx_rate_control\":0,\"rx_osr_control\":0,\"signal_control\":0,\"rx_equalizer\":0,\"rx_frontend_power\":0},\"pcs_counters\":{\"cw_start\":0,\"cw_end\":0,\"sof_to_mac\":0,\"eof_to_mac\":0,\"psync_mismatch\":0,\"sfc_hec_error\":0,\"pon_id_hec_error\":0}}\n"));
    assert(strstr(out,"\"rx_power_valid\":false,\"rx_power_nw\":null"));
    rx_restore_gain=true; assert(!q1000k_rx_bench_prepare() && phy_restore_gain);
    power_valid=true; assert(qrx_status_get(out,NULL)>0);
    assert(strstr(out,"\"gain_restore_enabled\":true"));
    assert(strstr(out,"\"rx_power_valid\":true,\"rx_power_nw\":19900"));
    rx_reacquire=false; assert(q1000k_rx_bench_prepare()==-EINVAL);
    return 0;
}
