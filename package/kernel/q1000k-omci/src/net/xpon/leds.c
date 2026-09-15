// SPDX-License-Identifier: GPL-2.0-only

#include <linux/err.h>
#include <linux/leds.h>

#include "internal.h"

static void xpon_led_set(struct led_classdev *led, enum led_brightness value)
{
	if (!led)
		return;
	/* All callers are sleepable. A nonzero brightness alone changes the
	 * blink brightness; it does not stop discovery blinking at O5.
	 */
	if (led->blink_delay_on || led->blink_delay_off) {
		led_set_brightness(led, LED_OFF);
		flush_work(&led->set_brightness_work);
	}
	led_set_brightness(led, value);
}

static struct led_classdev *
xpon_led_get_optional(struct xpon_device *xpon, char *name)
{
	struct led_classdev *led;

	/* The provider device can outlive many xPON registrations. */
	led = led_get(xpon->parent, name);
	if (IS_ERR(led) && PTR_ERR(led) == -ENOENT)
		return NULL;

	return led;
}

int xpon_leds_register(struct xpon_device *xpon)
{
	int ret;

	if (!xpon->pon_led) {
		xpon->pon_led = xpon_led_get_optional(xpon, "pon");
		if (IS_ERR(xpon->pon_led)) {
			ret = PTR_ERR(xpon->pon_led);
			xpon->pon_led = NULL;
			goto fail;
		}
		if (xpon->pon_led)
			xpon->owned_leds |= BIT(0);
	}

	if (!xpon->los_led) {
		xpon->los_led = xpon_led_get_optional(xpon, "los");
		if (IS_ERR(xpon->los_led)) {
			ret = PTR_ERR(xpon->los_led);
			xpon->los_led = NULL;
			goto fail;
		}
		if (xpon->los_led)
			xpon->owned_leds |= BIT(1);
	}

	if (!xpon->fiber_led) {
		xpon->fiber_led = xpon_led_get_optional(xpon, "fiber");
		if (IS_ERR(xpon->fiber_led)) {
			ret = PTR_ERR(xpon->fiber_led);
			xpon->fiber_led = NULL;
			goto fail;
		}
		if (xpon->fiber_led)
			xpon->owned_leds |= BIT(2);
	}

	return 0;
fail:
	xpon_leds_unregister(xpon);
	return ret;
}

void xpon_leds_unregister(struct xpon_device *xpon)
{
	struct led_classdev *leds[] = {
		xpon->pon_led, xpon->los_led, xpon->fiber_led,
	};
	unsigned int i;

	/* cancel_work_sync(notify_work) precedes this on registered devices. */
	for (i = 0; i < ARRAY_SIZE(leds); i++) {
		xpon_led_set(leds[i], LED_OFF);
		if (leds[i])
			flush_work(&leds[i]->set_brightness_work);
		if (xpon->owned_leds & BIT(i))
			led_put(leds[i]);
	}
	xpon->owned_leds = 0;
	xpon->pon_led = xpon->los_led = xpon->fiber_led = NULL;
}

void xpon_leds_update(struct xpon_device *xpon,
		      const struct xpon_state *state)
{
	unsigned long delay_on = 250;
	unsigned long delay_off = 250;

	if ((state->valid & XPON_STATE_F_LOS) && state->los) {
		xpon_led_set(xpon->fiber_led, LED_OFF);
		xpon_led_set(xpon->los_led, LED_FULL);
		xpon_led_set(xpon->pon_led, LED_OFF);
		return;
	}

	if (state->valid & XPON_STATE_F_SIGNAL)
		xpon_led_set(xpon->fiber_led,
			     state->signal_detect ? LED_FULL : LED_OFF);
	xpon_led_set(xpon->los_led, LED_OFF);

	switch (state->registration) {
	case XPON_REGISTRATION_OPERATIONAL:
		xpon_led_set(xpon->pon_led, LED_FULL);
		break;
	case XPON_REGISTRATION_DISCOVERY:
	case XPON_REGISTRATION_REGISTERING:
		if (xpon->pon_led)
			led_blink_set(xpon->pon_led, &delay_on, &delay_off);
		break;
	default:
		xpon_led_set(xpon->pon_led, LED_OFF);
		break;
	}
}
