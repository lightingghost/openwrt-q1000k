// SPDX-License-Identifier: GPL-2.0-only
/* Appended after the core fixture only for the disposable CLI UML guest. */
static struct xpon_device cli_xpon;
static struct omci_device *cli_omci;
static struct net_device *cli_netdev;
static struct device *cli_parent;
static netdev_tx_t cli_drop(struct sk_buff *skb, struct net_device *dev)
{
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}
static const struct net_device_ops cli_netdev_ops = { .ndo_start_xmit = cli_drop };

void q1000k_omci_cli_cleanup(void)
{
	if (cli_omci) {
		omci_device_unregister(cli_omci);
		cli_omci = NULL;
	}
	if (cli_netdev) {
		unregister_netdev(cli_netdev);
		free_netdev(cli_netdev);
		cli_netdev = NULL;
	}
	if (cli_parent) {
		root_device_unregister(cli_parent);
		cli_parent = NULL;
	}
}

int q1000k_omci_cli_setup(void)
{
	const u8 serial[8] = { 'T', 'E', 'S', 'T', 0, 0, 0, 1 };
	int ret;

	if (!strstr(init_utsname()->release, "-q1000k-omci-core-test"))
		return -EPERM;
	fixture_start_error = fixture_batch_error = fixture_scheduler_error = 0;
	cli_parent = root_device_register("q1000k-omci-cli-fixture");
	if (IS_ERR(cli_parent)) { ret = PTR_ERR(cli_parent); cli_parent = NULL; return ret; }
	cli_netdev = alloc_etherdev(0);
	if (!cli_netdev) { ret = -ENOMEM; goto fail; }
	strscpy(cli_netdev->name, "pon-test", IFNAMSIZ);
	cli_netdev->netdev_ops = &cli_netdev_ops;
	eth_hw_addr_random(cli_netdev);
	ret = register_netdev(cli_netdev);
	if (ret) { free_netdev(cli_netdev); cli_netdev = NULL; goto fail; }
	cli_xpon.class_dev = cli_parent;
	cli_xpon.netdev = cli_netdev;
	dev_set_drvdata(cli_parent, &cli_xpon);
	cli_omci = omci_device_register(&cli_xpon, OMCI_CAP_PROVIDER_MIC, &fixture_ops, NULL);
	if (IS_ERR(cli_omci)) { ret = PTR_ERR(cli_omci); cli_omci = NULL; goto fail; }
	omci_device_set_identity(cli_omci, serial, NULL);
	ret = omci_device_start(cli_omci);
	if (ret) goto fail;
	omci_device_set_onu_id(cli_omci, 7);
	omci_device_set_state(cli_omci, 5);
	omci_device_set_channel(cli_omci, 7, true);
	ret = omci_device_set_auth_epoch(cli_omci, ++fixture_auth_epoch);
	if (!ret) return 0;
fail:
	q1000k_omci_cli_cleanup();
	return ret;
}
