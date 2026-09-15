// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#define __exit
#define module_exit(fn) static void (*scu_unload)(void) = fn
#define ERR_PTR(error) ((void *)(intptr_t)(error))
#define IS_ERR(ptr) ((uintptr_t)(ptr) > (uintptr_t)-4096)
#define PTR_ERR(ptr) ((int)(intptr_t)(ptr))
#define CR_NP_SCU_SCREG_WR0 0x280
typedef uint32_t u32;
struct regmap { u32 value; };
static struct regmap np_map = { 0xa5a55a5a }, chip_map = { 0x12345678 };
static struct regmap *an7581_np_scu, *an7581_chip_scu;
static u32 l2c_sram_size;
static bool board = true;
static int np_error, chip_error, read_error, reads, lookups;

static bool of_machine_is_compatible(const char *compatible)
{
    assert(!strcmp(compatible, "quantum,q1000k-ubi"));
    return board;
}

static struct regmap *syscon_regmap_lookup_by_compatible(const char *compatible)
{
    lookups++;
    if (!strcmp(compatible, "airoha,en7581-scu"))
        return np_error ? ERR_PTR(np_error) : &np_map;
    assert(!strcmp(compatible, "airoha,en7581-chip-scu"));
    return chip_error ? ERR_PTR(chip_error) : &chip_map;
}

static int regmap_read(struct regmap *map, unsigned int reg, u32 *value)
{
    assert(map == &np_map && reg == CR_NP_SCU_SCREG_WR0);
    reads++;
    if (read_error)
        return read_error;
    *value = map->value;
    return 0;
}

/* No write, map release, clock/reset or IRQ operation is provided. */
/* PRODUCTION LIFECYCLE */

int main(void)
{
    assert(!ECNT_SCU_DRV_PROBE());
    assert(an7581_np_scu == &np_map && an7581_chip_scu == &chip_map);
    assert(l2c_sram_size == np_map.value && reads == 1 && lookups == 2);
    scu_unload();
    assert(!an7581_np_scu && !an7581_chip_scu && !l2c_sram_size);
    assert(reads == 1 && lookups == 2);
    assert(np_map.value == 0xa5a55a5a && chip_map.value == 0x12345678);

    for (int fault = 0; fault < 4; fault++) {
        board = fault != 0;
        np_error = fault == 1 ? -ENODEV : 0;
        chip_error = fault == 2 ? -ENODEV : 0;
        read_error = fault == 3 ? -EIO : 0;
        assert(ECNT_SCU_DRV_PROBE() == (fault == 3 ? -EIO : -ENODEV));
        assert(!an7581_np_scu && !an7581_chip_scu && !l2c_sram_size);
    }
    board = true;
    np_error = chip_error = read_error = 0;
    assert(!ECNT_SCU_DRV_PROBE());
    scu_unload();
    assert(!an7581_np_scu && !an7581_chip_scu && !l2c_sram_size);
    return 0;
}
