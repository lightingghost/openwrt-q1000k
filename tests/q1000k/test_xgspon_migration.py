#!/usr/bin/env python3
"""Real UCI legacy migration, staged edit isolation and canonical precedence."""
import subprocess
import unittest
import test_pon_config
import test_pon_config_watch

ROOT=test_pon_config.ROOT
class MigrationTests(unittest.TestCase):
    setUpClass=classmethod(test_pon_config_watch.ConfigWatchTests.setUpClass.__func__)
    uci_cmd=test_pon_config_watch.ConfigWatchTests.uci_cmd
    def setUp(self):
        test_pon_config.ConfigTests.setUp(self)
        self.config=self.root/'config/xgspon'
        self.legacy=self.root/'config/q1000k-xgspon'
        self.defaults=self.root/'defaults'; self.defaults.write_bytes(self.config.read_bytes())
        self.marker=self.root/'migrated'
        self.dt=self.root/'dt'; self.dt.mkdir()
        source=(ROOT/'package/network/utils/q1000k-xgspon/files/migrate-config').read_text()
        for a,b in [('/sys/firmware/devicetree/base/',str(self.dt)+'/'),('/etc/config/',str(self.root/'config')+'/'),('/usr/share/xgspon/defaults',str(self.defaults)),('/etc/xgspon-config-migrated',str(self.marker)),('/etc/xgspon-legacy-config',str(self.root/'backup')),('/tmp/xgspon-migrate-',str(self.root/'migration-'))]:
            source=source.replace(a,b)
        self.script=self.backend.write('migration',source)
        self.old="config identity 'identity'\n option registration_id '0123'\nconfig service 'service'\n option enabled '1'\n option continuous_bench '1'\n"
    def run_migration(self,rc=0):
        run=subprocess.run(['busybox','ash',str(self.script)],env=self.env,text=True,capture_output=True)
        self.assertEqual(run.returncode,rc,run.stderr)
    def test_legacy_identity_autostart_and_disabled_handoff_survive_once(self):
        self.legacy.write_text(self.old)
        self.uci_cmd('set','q1000k-xgspon.identity.registration_id=uncommitted')
        self.run_migration()
        self.assertEqual(self.uci_cmd('get','xgspon.identity.registration_id'),'0123')
        self.assertEqual(self.uci_cmd('get','xgspon.service.continuous_bench'),'1')
        self.assertEqual(self.uci_cmd('get','xgspon.passthrough.mode'),'router')
        self.assertEqual(self.config.stat().st_mode & 0o777,0o600)
        self.config.write_bytes(self.defaults.read_bytes()); self.run_migration()
        self.assertEqual(self.config.read_bytes(),self.defaults.read_bytes())
    def test_custom_canonical_wins_and_staged_canonical_values_stay_uncommitted(self):
        self.legacy.write_text(self.old)
        self.config.write_text(self.old.replace('0123','5678'))
        self.uci_cmd('set','xgspon.identity.registration_id=uncommitted')
        self.run_migration()
        self.assertIn("'5678'",self.config.read_text())
        self.assertNotIn('uncommitted',self.config.read_text())
        self.assertEqual(self.uci_cmd('get','xgspon.identity.registration_id'),'uncommitted')
    def test_preupgrade_backup_recovers_when_old_conffile_was_removed(self):
        (self.root/'backup').write_text(self.old); self.run_migration()
        self.assertEqual(self.uci_cmd('get','xgspon.identity.registration_id'),'0123')
    def test_apk_save_legacy_supported(self):
        self.legacy.with_name(self.legacy.name+'.apk-save').write_text(self.old)
        self.run_migration(); self.assertEqual(self.uci_cmd('get','xgspon.identity.registration_id'),'0123')
    def test_fresh_install_and_missing_canonical_default(self):
        self.config.unlink(); self.run_migration()
        self.assertEqual(self.config.read_bytes(),self.defaults.read_bytes())
    def test_normal_fresh_boot_enables_registration_but_not_passthrough(self):
        (self.dt/'quantum,xgspon-service').touch()
        self.run_migration()
        self.assertEqual(self.uci_cmd('get','xgspon.service.enabled'),'1')
        self.assertEqual(self.uci_cmd('get','xgspon.passthrough.mode'),'router')
        empty = subprocess.run([str(self.root/'uci'), '-q', 'get', 'xgspon.identity.registration_id'], env=self.env, text=True, capture_output=True)
        self.assertEqual(empty.stdout, '')
        self.uci_cmd('set','xgspon.service.enabled=0')
        self.uci_cmd('commit','xgspon')
        self.run_migration()
        self.assertEqual(self.uci_cmd('get','xgspon.service.enabled'),'0')
    def test_bench_keeps_monitor_only_even_if_normal_marker_is_inherited(self):
        for name in ['quantum,xgspon-service','quantum,xgspon-bench']:
            (self.dt/name).touch()
        self.run_migration()
        self.assertEqual(self.uci_cmd('get','xgspon.service.enabled'),'0')
    def test_normal_upgrade_preserves_configured_disabled_identity_and_staging(self):
        (self.dt/'quantum,xgspon-service').touch()
        self.config.write_text(self.old.replace("enabled '1'", "enabled '0'"))
        self.uci_cmd('set','xgspon.identity.registration_id=uncommitted')
        self.run_migration()
        self.assertEqual(self.uci_cmd('get','xgspon.service.enabled'),'0')
        self.assertIn("'0123'",self.config.read_text())
        self.assertNotIn('uncommitted',self.config.read_text())
    def test_normal_migration_preserves_legacy_disabled_choice(self):
        (self.dt/'quantum,xgspon-service').touch()
        self.legacy.write_text(self.old.replace("enabled '1'", "enabled '0'"))
        self.run_migration()
        self.assertEqual(self.uci_cmd('get','xgspon.service.enabled'),'0')
    def test_malformed_legacy_not_published(self):
        self.legacy.write_text("config 'broken\n"); self.run_migration(1)
        self.assertEqual(self.config.read_bytes(),self.defaults.read_bytes())
        self.assertFalse(self.marker.exists())

if __name__=='__main__':unittest.main()
