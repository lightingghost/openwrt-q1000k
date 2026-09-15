// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#define BIT(n) (1U<<(n))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define IS_ERR(p) ((uintptr_t)(p)>=(uintptr_t)-4095)
#define PTR_ERR(p) ((intptr_t)(p))
#define ERR_PTR(n) ((void *)(intptr_t)(n))
#define EPROBE_DEFER 517
#define XPON_STATE_F_LOS 1
#define XPON_STATE_F_SIGNAL 2
#define XPON_REGISTRATION_OPERATIONAL 3
#define XPON_REGISTRATION_DISCOVERY 1
#define XPON_REGISTRATION_REGISTERING 2
struct xpon_state { unsigned int valid; bool los,signal_detect; int registration; };
enum led_brightness { LED_OFF=0, LED_FULL=255 };
struct work_struct { bool pending; };
struct led_classdev {
    struct work_struct set_brightness_work;
    unsigned long blink_delay_on,blink_delay_off;
    int brightness,refs;
};
struct xpon_device { void *parent; struct led_classdev *pon_led,*los_led,*fiber_led; unsigned int owned_leds; };
static struct led_classdev leds[3];
static int fail_index=-1,missing_index=-1;
static struct led_classdev *led_get(void *dev,char *name) {
    int i=!strcmp(name,"pon")?0:!strcmp(name,"los")?1:2;
    if(i==fail_index) return ERR_PTR(-EPROBE_DEFER);
    if(i==missing_index) return ERR_PTR(-ENOENT);
    leds[i].refs++; return &leds[i];
}
static void led_put(struct led_classdev *l) { assert(l->refs>0); l->refs--; }
static void led_set_brightness(struct led_classdev *l,enum led_brightness value) {
    if(l->blink_delay_on) {
        if(!value) l->set_brightness_work.pending=true;
        return; /* A nonzero value must not cancel software blinking. */
    }
    l->brightness=value;
}
static void flush_work(struct work_struct *w) {
    struct led_classdev *l=(void *)((char *)w - offsetof(struct led_classdev,set_brightness_work));
    if(w->pending) { w->pending=false; l->blink_delay_on=l->blink_delay_off=0; l->brightness=0; }
}
static void led_blink_set(struct led_classdev *l,unsigned long *on,unsigned long *off) {
    l->blink_delay_on=*on; l->blink_delay_off=*off;
}
void xpon_leds_unregister(struct xpon_device *xpon);
/* PRODUCTION */
int main(void) {
    for(int cycle=0;cycle<100;cycle++) {
        struct xpon_device x={0};
        assert(!xpon_leds_register(&x));
        struct xpon_state s={.valid=3,.los=true,.registration=XPON_REGISTRATION_DISCOVERY};
        xpon_leds_update(&x,&s);
        assert(leds[1].brightness==255 && !leds[0].brightness && !leds[2].brightness);
        s.los=false; s.signal_detect=true; xpon_leds_update(&x,&s);
        assert(!leds[1].brightness && leds[0].blink_delay_on==250 && leds[2].brightness==255);
        s.registration=XPON_REGISTRATION_OPERATIONAL; xpon_leds_update(&x,&s);
        assert(!leds[0].blink_delay_on && leds[0].brightness==255);
        s.registration=XPON_REGISTRATION_REGISTERING; xpon_leds_update(&x,&s);
        assert(leds[0].blink_delay_on==250);
        s.los=true; xpon_leds_update(&x,&s);
        assert(!leds[0].blink_delay_on && !leds[0].brightness && leds[1].brightness==255);
        xpon_leds_unregister(&x);
        for(int i=0;i<3;i++) assert(!leds[i].refs && !leds[i].brightness);
    }
    for(fail_index=0;fail_index<3;fail_index++) {
        struct xpon_device x={0}; assert(xpon_leds_register(&x)==-EPROBE_DEFER);
        for(int i=0;i<3;i++) assert(!leds[i].refs);
    }
    fail_index=-1; missing_index=2;
    struct led_classdev borrowed={0}; struct xpon_device x={.pon_led=&borrowed};
    assert(!xpon_leds_register(&x) && !x.fiber_led && x.owned_leds==BIT(1));
    xpon_leds_unregister(&x); assert(!borrowed.refs);
    return 0;
}
