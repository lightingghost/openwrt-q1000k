#!/usr/bin/env python3
"""Reproduce the board's GPIO request using the actual Linux pinmux core."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
from pon_test_utils import run_c

ROOT = Path(__file__).resolve().parents[2]


class PinmuxTests(unittest.TestCase):
    def test_gpio_mux_then_descriptor_preserves_strict_peripheral_ownership(self):
        kernels = list((ROOT / 'build_dir/target-aarch64_cortex-a53_musl/linux-airoha_an7581').glob('linux-6.18.*'))
        self.assertEqual(len(kernels), 1)
        kernel = kernels[0]
        relative = 'drivers/pinctrl/airoha/'
        with tempfile.TemporaryDirectory(prefix='q1000k-pinmux-test.') as temporary:
            temporary = Path(temporary)
            (temporary / relative).mkdir(parents=True)
            for name in ('pinctrl-airoha.c', 'pinctrl-an7581.c'):
                shutil.copyfile(kernel / relative / name, temporary / relative / name)
            if '.function_is_gpio = pinmux_generic_function_is_gpio' not in (temporary / relative / 'pinctrl-airoha.c').read_text():
                subprocess.run(['patch', '--batch', '-p1', '-i', str(ROOT / 'target/linux/airoha/patches-6.18/9999k-pinctrl-airoha-identify-an7581-gpio-function.patch')],
                               cwd=temporary, check=True, capture_output=True)
            common = (temporary / relative / 'pinctrl-airoha.c').read_text()
            soc = (temporary / relative / 'pinctrl-an7581.c').read_text()
        ops = common[common.index('static const struct pinmux_ops airoha_pmxops'):]
        ops = ops[:ops.index('};')]
        self.assertIn('.function_is_gpio = pinmux_generic_function_is_gpio', ops)
        self.assertIn('.strict = true', ops)
        # GPIO38 must be selectable as well as having a mux definition.
        # The reduced PON group must not claim the same pin as its GPIO owner.
        groups = re.search(r'static const char \*const gpio_groups\[\] = \{([\s\S]*?)\};', soc)[1]
        if 'pon_sw_tx_pins' in soc:
            self.assertIn('"gpio38"', groups)
            gpio_pin = int(re.search(r'gpio38_pins\[\] = \{ (\d+) \}', soc)[1])
            peripheral_pins = [int(x) for x in re.search(r'pon_sw_tx_pins\[\] = \{ ([^}]+) \}', soc)[1].split(',')]
            self.assertNotIn(gpio_pin, peripheral_pins)
            self.assertEqual(gpio_pin, 51)
        descriptor = re.search(r'\{\n\t\t.desc = PINCTRL_GPIO_PINFUNCTION\("gpio",[\s\S]*?\n\t\}', soc)
        self.assertIsNotNone(descriptor)
        header = (kernel / 'include/linux/pinctrl/pinctrl.h').read_text()
        macro = header[header.index('#define PINCTRL_GPIO_PINFUNCTION'):].split('\n\n', 1)[0]
        core = (kernel / 'drivers/pinctrl/pinmux.c').read_text()
        can_use = core[core.index('bool pinmux_can_be_used_for_gpio('):core.index('/**\n * pin_request()')]
        request = core[core.index('static int pin_request('):core.index('/**\n * pin_free()')]
        classify = core[core.index('bool pinmux_generic_function_is_gpio('):core.index('EXPORT_SYMBOL_GPL(pinmux_generic_function_is_gpio)')]
        fixture = Path(__file__).with_name('pon_pinmux_fixture.c').read_text()
        source = fixture.replace('/* PINFUNCTION MACRO */', macro).replace('/* GPIO DESCRIPTOR */', descriptor.group(0))
        source = source.replace('/* PRODUCTION CORE */', classify + can_use + request)
        run_c(source)


if __name__ == '__main__':
    unittest.main()
