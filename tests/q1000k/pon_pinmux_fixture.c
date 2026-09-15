// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define PINFUNCTION_FLAG_GPIO 1UL
#define dev_err(...) ((void)0)
#define dev_dbg(...) ((void)0)
#define dev_err_probe(...) ((void)0)
#define guard(t) (void)
#define scoped_guard(t,p) for (int once = ((void)(p), 1); once; once = 0)
struct pinfunction { const char *name; const char *const *groups; size_t ngroups; unsigned long flags; };
/* PINFUNCTION MACRO */
struct airoha_pinctrl_func { struct pinfunction desc; const int *groups; size_t group_size; };
static const char *const gpio_groups[] = { "gpio9" };
static const int gpio_func_group[] = { 0 };
static const struct airoha_pinctrl_func gpio_func = /* GPIO DESCRIPTOR */;
struct pinctrl_dev;
struct pinctrl_gpio_range { int unused; };
struct pinctrl_setting_mux { unsigned int func; };
struct pin_desc {
    const char *name, *mux_owner, *gpio_owner;
    unsigned int mux_usecount;
    int mux_lock;
    const struct pinctrl_setting_mux *mux_setting;
};
struct pinmux_ops {
    bool strict;
    bool (*function_is_gpio)(struct pinctrl_dev *, unsigned int);
    int (*gpio_request_enable)(struct pinctrl_dev *, struct pinctrl_gpio_range *, unsigned int);
    int (*request)(struct pinctrl_dev *, unsigned int);
};
struct pinctrl_desc { const struct pinmux_ops *pmxops; };
struct function_desc { const struct pinfunction *func; };
struct pinctrl_dev { struct pinctrl_desc *desc; struct function_desc *pin_function_tree; int owner; };
static struct pin_desc pin;
static struct pin_desc *pin_desc_get(struct pinctrl_dev *dev, unsigned int p)
{ (void)dev; return p == 22 ? &pin : NULL; }
static struct function_desc *radix_tree_lookup(struct function_desc **tree, unsigned int selector)
{ return selector < 2 ? &(*tree)[selector] : NULL; }
static int refs;
static bool try_module_get(int owner) { (void)owner; refs++; return true; }
static void module_put(int owner) { (void)owner; refs--; }
/* PRODUCTION CORE */

int main(void)
{
    struct pinfunction peripheral = { .name = "pon" };
    struct function_desc functions[] = { { &gpio_func.desc }, { &peripheral } };
    struct pinmux_ops ops = { .strict = true };
    struct pinctrl_desc desc = { &ops };
    struct pinctrl_dev dev = { .desc = &desc, .pin_function_tree = functions };
    struct pinctrl_setting_mux selected = { .func = 0 };
    struct pinctrl_gpio_range range = {};
    pin = (struct pin_desc){ .name = "gpio9", .mux_owner = "0-0051", .mux_usecount = 1, .mux_setting = &selected };

    /* Exact live failure: mux owner and GPIO owner labels differ. */
    assert(!pinmux_can_be_used_for_gpio(&dev, 22));
    assert(pin_request(&dev, 22, "pinctrl:521", &range) == -EINVAL);
    assert(!pin.gpio_owner && refs == 0);

    ops.function_is_gpio = pinmux_generic_function_is_gpio;
    assert(pinmux_generic_function_is_gpio(&dev, 0));
    assert(!pinmux_generic_function_is_gpio(&dev, 1));
    assert(!pinmux_generic_function_is_gpio(&dev, 2));
    assert(pinmux_can_be_used_for_gpio(&dev, 22));
    assert(!pin_request(&dev, 22, "pinctrl:521", &range));
    assert(!strcmp(pin.gpio_owner, "pinctrl:521") && refs == 1);
    assert(pin.mux_usecount == 1 && !strcmp(pin.mux_owner, "0-0051"));
    assert(!pinmux_can_be_used_for_gpio(&dev, 22));

    /* A real peripheral mux still rejects both GPIO and other owners. */
    pin.gpio_owner = NULL;
    selected.func = 1;
    assert(!pinmux_can_be_used_for_gpio(&dev, 22));
    assert(pin_request(&dev, 22, "pinctrl:521", &range) == -EINVAL);
    assert(pin_request(&dev, 22, "another-peripheral", NULL) == -EINVAL);
    assert(refs == 1 && !pin.gpio_owner && pin.mux_usecount == 1);
    return 0;
}
