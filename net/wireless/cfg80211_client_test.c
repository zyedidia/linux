// SPDX-License-Identifier: GPL-2.0-only
/*
 * A test of the cfg80211 in-kernel client API. Writing "<interface> <SSID>
 * <MHz>" to cfg80211_client_test/run in debugfs scans, connects to the open
 * network with that SSID on that frequency, installs and removes a group key,
 * and disconnects. It then empties the BSS table and connects by SSID alone,
 * so that the SME scans. While the interface is down, it changes and restores
 * the MAC address. The write fails if the test fails, and the kernel log has
 * the details.
 */

#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/etherdevice.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <net/cfg80211.h>
#include <net/net_namespace.h>

struct client_test {
	struct completion scan_done;
	struct completion connected;
	struct completion disconnected;
	bool aborted;
	u16 status;
	u16 reason;
	bool locally_generated;
};

static void client_test_scan_done(void *priv, bool aborted)
{
	struct client_test *t = priv;

	t->aborted = aborted;
	complete(&t->scan_done);
}

static void
client_test_connect_result(void *priv,
			   const struct cfg80211_connect_resp_params *cr)
{
	struct client_test *t = priv;

	t->status = cr->status;
	complete(&t->connected);
}

static void client_test_disconnected(void *priv, u16 reason, const u8 *ie,
				     size_t ie_len, bool locally_generated)
{
	struct client_test *t = priv;

	t->reason = reason;
	t->locally_generated = locally_generated;
	complete(&t->disconnected);
}

static const struct cfg80211_client_ops client_test_ops = {
	.scan_done = client_test_scan_done,
	.connect_result = client_test_connect_result,
	.disconnected = client_test_disconnected,
};

static bool client_test_bss_matches(struct cfg80211_bss *bss, const char *ssid,
				    u32 freq)
{
	const struct element *elem;
	bool match = false;

	if (bss->channel->center_freq != freq)
		return false;

	rcu_read_lock();
	elem = ieee80211_bss_get_elem(bss, WLAN_EID_SSID);
	if (elem && elem->datalen == strlen(ssid) &&
	    !memcmp(elem->data, ssid, elem->datalen))
		match = true;
	rcu_read_unlock();
	return match;
}

/*
 * Find the network in the client's scan results, and record its BSSID and
 * channel.
 */
static int client_test_find(struct cfg80211_client *client,
			    struct wiphy *wiphy, const char *ssid, u32 freq,
			    u8 *bssid, struct ieee80211_channel **chan)
{
	struct cfg80211_bss **bss;
	int i, n, err = -ENOENT;

	n = cfg80211_client_get_scan_bss(client, &bss);
	if (n < 0)
		return n;

	pr_info("cfg80211_client_test: the scan found %d BSSes\n", n);
	for (i = 0; i < n; i++) {
		if (err && client_test_bss_matches(bss[i], ssid, freq)) {
			ether_addr_copy(bssid, bss[i]->bssid);
			*chan = bss[i]->channel;
			err = 0;
		}
		cfg80211_put_bss(wiphy, bss[i]);
	}
	kfree(bss);
	return err;
}

static int client_test_keys(struct cfg80211_client *client, const u8 *bssid)
{
	static const u8 key[16] = { 1, 2, 3, 4, 5, 6, 7, 8,
				    9, 10, 11, 12, 13, 14, 15, 16 };
	static const u8 seq[6] = {};
	/* One byte longer than nl80211 allows. */
	static const u8 long_seq[17] = {};
	struct key_params params = {
		.key = key,
		.key_len = sizeof(key),
		.seq = seq,
		.seq_len = sizeof(seq),
		.cipher = WLAN_CIPHER_SUITE_CCMP,
	};
	int err;

	err = cfg80211_client_authorize(client, bssid, true);
	if (err) {
		pr_err("cfg80211_client_test: authorize failed: %d\n", err);
		return err;
	}

	err = cfg80211_client_add_key(client, 1, false, NULL, &params);
	if (err) {
		pr_err("cfg80211_client_test: add_key failed: %d\n", err);
		return err;
	}

	err = cfg80211_client_del_key(client, 1, false, NULL);
	if (err) {
		pr_err("cfg80211_client_test: del_key failed: %d\n", err);
		return err;
	}

	params.seq = long_seq;
	params.seq_len = sizeof(long_seq);
	err = cfg80211_client_add_key(client, 1, false, NULL, &params);
	if (err != -EINVAL) {
		pr_err("cfg80211_client_test: a long sequence returned %d\n", err);
		return -EINVAL;
	}

	err = cfg80211_client_set_default_key(client, 4, true, true);
	if (err != -EINVAL) {
		pr_err("cfg80211_client_test: default key 4 returned %d\n", err);
		return -EINVAL;
	}

	err = cfg80211_client_set_default_mgmt_key(client, 0);
	if (err != -EINVAL) {
		pr_err("cfg80211_client_test: management key 0 returned %d\n",
		       err);
		return -EINVAL;
	}

	return 0;
}

/* Connections that nl80211's policy would refuse fail before they start. */
static int client_test_bad_connect(struct cfg80211_client *client,
				   const struct cfg80211_connect_params *good)
{
	static const u8 truncated_ie[] = { WLAN_EID_VENDOR_SPECIFIC, 5, 0 };
	struct cfg80211_connect_params connect = *good;
	int err;

	connect.ie = truncated_ie;
	connect.ie_len = sizeof(truncated_ie);
	err = cfg80211_client_connect(client, &connect);
	if (err != -EINVAL) {
		pr_err("cfg80211_client_test: malformed IEs returned %d\n", err);
		return -EINVAL;
	}

	connect = *good;
	connect.crypto.wpa_versions = BIT(3);
	err = cfg80211_client_connect(client, &connect);
	if (err != -EINVAL) {
		pr_err("cfg80211_client_test: an unknown WPA version returned %d\n",
		       err);
		return -EINVAL;
	}

	return 0;
}

/* Change the address of the interface while it is down, then restore it. */
static int client_test_mac(struct cfg80211_client *client,
			   struct net_device *dev)
{
	u8 orig[ETH_ALEN], addr[ETH_ALEN];
	int err;

	ether_addr_copy(orig, dev->dev_addr);
	ether_addr_copy(addr, orig);
	addr[5] ^= 0x10;

	err = cfg80211_client_set_mac(client, addr);
	if (err || !ether_addr_equal(dev->dev_addr, addr)) {
		pr_err("cfg80211_client_test: set_mac failed: %d\n", err);
		return err ?: -EIO;
	}

	err = cfg80211_client_set_mac(client, orig);
	if (err) {
		pr_err("cfg80211_client_test: restoring the address failed: %d\n",
		       err);
		return err;
	}

	return 0;
}

static int client_test_disconnect(struct cfg80211_client *client,
				  struct client_test *t)
{
	int err;

	reinit_completion(&t->disconnected);
	err = cfg80211_client_disconnect(client, WLAN_REASON_DEAUTH_LEAVING);
	if (err) {
		pr_err("cfg80211_client_test: disconnect failed: %d\n", err);
		return err;
	}
	if (!wait_for_completion_timeout(&t->disconnected, 10 * HZ) ||
	    !t->locally_generated) {
		pr_err("cfg80211_client_test: no local disconnection\n");
		return -ETIMEDOUT;
	}
	return 0;
}

/*
 * Flush the network from the BSS table with a scan of another channel of the
 * band, then connect by SSID alone. With no entry for the network, the SME
 * has to scan for it, and its scan must not be reported to the client.
 */
static int client_test_sme_scan(struct cfg80211_client *client,
				struct wiphy *wiphy, struct client_test *t,
				const char *ssid, u32 freq,
				struct ieee80211_channel *chan)
{
	struct ieee80211_supported_band *sband = wiphy->bands[chan->band];
	struct cfg80211_client_scan_params scan = {
		.flags = NL80211_SCAN_FLAG_FLUSH,
		.n_freqs = 1,
	};
	struct cfg80211_connect_params connect = {
		.ssid = ssid,
		.ssid_len = strlen(ssid),
		.auth_type = NL80211_AUTHTYPE_OPEN_SYSTEM,
		.mfp = NL80211_MFP_NO,
		.bg_scan_period = -1,
	};
	struct ieee80211_channel *other;
	struct cfg80211_bss *bss = NULL;
	u32 other_freq = 0;
	int err, i;

	for (i = 0; i < sband->n_channels; i++) {
		other = &sband->channels[i];
		if (other->center_freq != freq &&
		    !(other->flags & IEEE80211_CHAN_DISABLED)) {
			other_freq = other->center_freq;
			break;
		}
	}
	if (!other_freq) {
		pr_err("cfg80211_client_test: no other channel to flush with\n");
		return -ENOENT;
	}

	/*
	 * A beacon that arrives before the radio leaves its channel refreshes
	 * the entry, so the flush is repeated until the SME's lookup fails.
	 */
	scan.freqs = &other_freq;
	for (i = 0; i < 5; i++) {
		reinit_completion(&t->scan_done);
		err = cfg80211_client_scan(client, &scan);
		if (err) {
			pr_err("cfg80211_client_test: the flush scan failed: %d\n",
			       err);
			return err;
		}
		if (!wait_for_completion_timeout(&t->scan_done, 30 * HZ) ||
		    t->aborted) {
			pr_err("cfg80211_client_test: the flush scan did not complete\n");
			return -ETIMEDOUT;
		}

		bss = cfg80211_get_bss(wiphy, NULL, NULL, ssid, strlen(ssid),
				       IEEE80211_BSS_TYPE_ESS,
				       IEEE80211_PRIVACY_ANY);
		if (!bss)
			break;
		cfg80211_put_bss(wiphy, bss);
	}
	if (bss) {
		pr_err("cfg80211_client_test: the flush left %s\n", ssid);
		return -EINVAL;
	}
	pr_info("cfg80211_client_test: flush scan %d removed %s\n", i + 1,
		ssid);

	reinit_completion(&t->scan_done);
	reinit_completion(&t->connected);
	err = cfg80211_client_connect(client, &connect);
	if (err) {
		pr_err("cfg80211_client_test: connect by SSID failed: %d\n", err);
		return err;
	}
	if (!wait_for_completion_timeout(&t->connected, 30 * HZ) ||
	    t->status != WLAN_STATUS_SUCCESS) {
		pr_err("cfg80211_client_test: connect by SSID did not succeed\n");
		return -EIO;
	}
	if (completion_done(&t->scan_done)) {
		pr_err("cfg80211_client_test: a scan of the SME was reported\n");
		return -EINVAL;
	}
	pr_info("cfg80211_client_test: connected by SSID after an SME scan\n");

	return client_test_disconnect(client, t);
}

static int client_test_steps(struct cfg80211_client *client,
			     struct net_device *dev, struct client_test *t,
			     const char *ssid, u32 freq)
{
	struct cfg80211_client_scan_params scan = {};
	struct cfg80211_connect_params connect = {};
	struct wiphy *wiphy = dev->ieee80211_ptr->wiphy;
	struct ieee80211_channel *chan;
	struct station_info sinfo;
	u8 bssid[ETH_ALEN];
	int err;

	err = cfg80211_client_scan(client, &scan);
	if (err != -ENETDOWN) {
		pr_err("cfg80211_client_test: a scan while down returned %d\n",
		       err);
		return -EINVAL;
	}

	err = cfg80211_client_open(client);
	if (err) {
		pr_err("cfg80211_client_test: open failed: %d\n", err);
		return err;
	}

	err = cfg80211_client_scan(client, &scan);
	if (err) {
		pr_err("cfg80211_client_test: scan failed: %d\n", err);
		return err;
	}

	err = cfg80211_client_scan(client, &scan);
	if (err != -EBUSY) {
		pr_err("cfg80211_client_test: a second scan returned %d\n", err);
		return -EINVAL;
	}

	if (!wait_for_completion_timeout(&t->scan_done, 30 * HZ) ||
	    t->aborted) {
		pr_err("cfg80211_client_test: the scan did not complete\n");
		return -ETIMEDOUT;
	}

	err = cfg80211_client_abort_scan(client);
	if (err != -ENOENT) {
		pr_err("cfg80211_client_test: aborting a finished scan returned %d\n",
		       err);
		return -EINVAL;
	}

	err = client_test_find(client, wiphy, ssid, freq, bssid, &chan);
	if (err) {
		pr_err("cfg80211_client_test: %s on %u MHz was not found: %d\n",
		       ssid, freq, err);
		return err;
	}

	connect.ssid = ssid;
	connect.ssid_len = strlen(ssid);
	connect.bssid = bssid;
	connect.channel = chan;
	connect.auth_type = NL80211_AUTHTYPE_OPEN_SYSTEM;
	connect.mfp = NL80211_MFP_NO;
	connect.bg_scan_period = -1;

	err = client_test_bad_connect(client, &connect);
	if (err)
		return err;

	err = cfg80211_client_connect(client, &connect);
	if (err) {
		pr_err("cfg80211_client_test: connect failed: %d\n", err);
		return err;
	}
	if (!wait_for_completion_timeout(&t->connected, 10 * HZ)) {
		pr_err("cfg80211_client_test: no connect result\n");
		return -ETIMEDOUT;
	}
	if (t->status != WLAN_STATUS_SUCCESS) {
		pr_err("cfg80211_client_test: the connection failed: %u\n",
		       t->status);
		return -EIO;
	}
	pr_info("cfg80211_client_test: connected to %pM\n", bssid);

	err = cfg80211_client_get_station(client, bssid, &sinfo);
	if (err) {
		pr_err("cfg80211_client_test: get_station failed: %d\n", err);
		return err;
	}
	pr_info("cfg80211_client_test: signal %d dBm\n", sinfo.signal);
	cfg80211_sinfo_release_content(&sinfo);

	err = client_test_keys(client, bssid);
	if (err)
		return err;

	err = client_test_disconnect(client, t);
	if (err)
		return err;

	err = client_test_sme_scan(client, wiphy, t, ssid, freq, chan);
	if (err)
		return err;

	err = cfg80211_client_close(client);
	if (err) {
		pr_err("cfg80211_client_test: close failed: %d\n", err);
		return err;
	}

	err = cfg80211_client_scan(client, &scan);
	if (err != -ENETDOWN) {
		pr_err("cfg80211_client_test: a scan after close returned %d\n",
		       err);
		return -EINVAL;
	}

	return client_test_mac(client, dev);
}

static int client_test_run(const char *val)
{
	char ifname[IFNAMSIZ], ssid[IEEE80211_MAX_SSID_LEN + 1];
	struct cfg80211_client *client;
	struct net_device *dev;
	struct client_test t;
	u32 freq;
	int err;

	if (sscanf(val, "%15s %32s %u", ifname, ssid, &freq) != 3)
		return -EINVAL;

	dev = dev_get_by_name(&init_net, ifname);
	if (!dev)
		return -ENODEV;
	if (!dev->ieee80211_ptr) {
		dev_put(dev);
		return -EINVAL;
	}

	init_completion(&t.scan_done);
	init_completion(&t.connected);
	init_completion(&t.disconnected);

	client = cfg80211_client_register(dev, &client_test_ops, &t);
	if (IS_ERR(client)) {
		err = PTR_ERR(client);
		pr_err("cfg80211_client_test: register failed: %d\n", err);
	} else {
		err = client_test_steps(client, dev, &t, ssid, freq);
		/* A failed run disconnects and closes the interface. */
		if (err) {
			cfg80211_client_disconnect(client,
						   WLAN_REASON_DEAUTH_LEAVING);
			cfg80211_client_close(client);
		}
		cfg80211_client_unregister(client);
	}
	dev_put(dev);

	pr_info("cfg80211_client_test: %s\n", err ? "failed" : "passed");
	return err;
}

static ssize_t client_test_write(struct file *file, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	char val[64];
	int err;

	if (count >= sizeof(val))
		return -EINVAL;
	if (copy_from_user(val, buf, count))
		return -EFAULT;
	val[count] = 0;

	err = client_test_run(val);
	return err ?: count;
}

static const struct file_operations client_test_fops = {
	.write = client_test_write,
	.llseek = noop_llseek,
};

static struct dentry *client_test_dir;

static int __init client_test_init(void)
{
	client_test_dir = debugfs_create_dir("cfg80211_client_test", NULL);
	debugfs_create_file("run", 0200, client_test_dir, NULL,
			    &client_test_fops);
	return 0;
}
module_init(client_test_init);

static void __exit client_test_exit(void)
{
	debugfs_remove_recursive(client_test_dir);
}
module_exit(client_test_exit);

MODULE_DESCRIPTION("Test of the cfg80211 in-kernel client API");
MODULE_LICENSE("GPL");
