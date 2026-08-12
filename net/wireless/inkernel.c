// SPDX-License-Identifier: GPL-2.0
/*
 * cfg80211 in-kernel consumer.
 *
 * Submits requests through the same rdev ops nl80211 uses, and reports
 * results back through callbacks driven from the notification entry
 * points drivers already call. See include/net/cfg80211_ik.h.
 */

#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/slab.h>
#include <net/cfg80211.h>
#include <net/cfg80211_ik.h>

#include "core.h"
#include "nl80211.h"
#include "rdev-ops.h"

struct cfg80211_ik {
	/* Cleared when the interface is unregistered; read with READ_ONCE. */
	struct wireless_dev *wdev;
	struct net_device *netdev;
	const struct cfg80211_ik_ops *ops;
	void *priv;
};

/*
 * A netdev reference does not pin an interface -- it only makes
 * unregister_netdevice() wait, forever, for a reference this API would
 * never drop. Track the unregister instead and let go.
 */
static struct wireless_dev *ik_wdev(struct cfg80211_ik *ik)
{
	return READ_ONCE(ik->wdev);
}

/*
 * A single consumer, published with RCU so that the notification hooks
 * can look it up from any context without taking a lock.
 */
static struct cfg80211_ik __rcu *ik_consumer;
static DEFINE_MUTEX(ik_attach_lock);

static struct wireless_dev *ik_find_wdev(const char *ifname)
{
	struct cfg80211_registered_device *rdev;
	struct wireless_dev *wdev;

	list_for_each_entry(rdev, &cfg80211_rdev_list, list) {
		list_for_each_entry(wdev, &rdev->wiphy.wdev_list, list) {
			if (wdev->iftype != NL80211_IFTYPE_STATION)
				continue;
			if (!wdev->netdev)
				continue;
			if (ifname && strcmp(wdev->netdev->name, ifname))
				continue;
			return wdev;
		}
	}
	return NULL;
}

static int ik_netdev_event(struct notifier_block *nb, unsigned long state,
			   void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	struct cfg80211_ik *ik;

	if (state != NETDEV_UNREGISTER)
		return NOTIFY_DONE;

	guard(mutex)(&ik_attach_lock);

	ik = rcu_dereference_protected(ik_consumer,
				       lockdep_is_held(&ik_attach_lock));
	if (!ik || ik->netdev != dev)
		return NOTIFY_DONE;

	/*
	 * Let the consumer stop first, then release the reference that
	 * unregister_netdevice() is waiting on.
	 */
	if (ik->ops->iface_gone)
		ik->ops->iface_gone(ik->priv);

	WRITE_ONCE(ik->wdev, NULL);
	dev_put(ik->netdev);
	ik->netdev = NULL;

	return NOTIFY_DONE;
}

static struct notifier_block ik_netdev_nb = {
	.notifier_call = ik_netdev_event,
};

struct cfg80211_ik *cfg80211_ik_attach(const char *ifname,
				       const struct cfg80211_ik_ops *ops,
				       void *priv)
{
	struct wireless_dev *wdev;
	struct cfg80211_ik *ik;

	if (!ops)
		return ERR_PTR(-EINVAL);

	guard(mutex)(&ik_attach_lock);

	if (rcu_access_pointer(ik_consumer))
		return ERR_PTR(-EBUSY);

	ik = kzalloc(sizeof(*ik), GFP_KERNEL);
	if (!ik)
		return ERR_PTR(-ENOMEM);

	rtnl_lock();
	wdev = ik_find_wdev(ifname);
	if (wdev) {
		ik->wdev = wdev;
		ik->netdev = wdev->netdev;
		dev_hold(ik->netdev);
	}
	rtnl_unlock();

	if (!ik->wdev) {
		kfree(ik);
		return ERR_PTR(-EPROBE_DEFER);
	}

	ik->ops = ops;
	ik->priv = priv;

	rcu_assign_pointer(ik_consumer, ik);

	/* Outside the RTNL: registering walks the device list itself. */
	if (register_netdevice_notifier(&ik_netdev_nb)) {
		rcu_assign_pointer(ik_consumer, NULL);
		synchronize_rcu();
		dev_put(ik->netdev);
		kfree(ik);
		return ERR_PTR(-ENOMEM);
	}

	return ik;
}
EXPORT_SYMBOL_GPL(cfg80211_ik_attach);

void cfg80211_ik_detach(struct cfg80211_ik *ik)
{
	if (IS_ERR_OR_NULL(ik))
		return;

	unregister_netdevice_notifier(&ik_netdev_nb);

	scoped_guard(mutex, &ik_attach_lock)
		rcu_assign_pointer(ik_consumer, NULL);

	/* Callbacks may be running; wait them out before freeing. */
	synchronize_rcu();

	if (ik->netdev)
		dev_put(ik->netdev);
	kfree(ik);
}
EXPORT_SYMBOL_GPL(cfg80211_ik_detach);

struct wiphy *cfg80211_ik_wiphy(struct cfg80211_ik *ik)
{
	struct wireless_dev *wdev = ik_wdev(ik);

	return wdev ? wdev->wiphy : NULL;
}
EXPORT_SYMBOL_GPL(cfg80211_ik_wiphy);

struct net_device *cfg80211_ik_netdev(struct cfg80211_ik *ik)
{
	struct wireless_dev *wdev = ik_wdev(ik);

	return wdev ? wdev->netdev : NULL;
}
EXPORT_SYMBOL_GPL(cfg80211_ik_netdev);

/* Requests */

int cfg80211_ik_scan(struct cfg80211_ik *ik,
		     const struct cfg80211_ssid *ssids, int n_ssids,
		     const u32 *freqs, int n_freqs,
		     const u8 *ie, size_t ie_len)
{
	struct cfg80211_registered_device *rdev;
	struct cfg80211_scan_request_int *request;
	struct wireless_dev *wdev = ik_wdev(ik);
	int n_channels, i, j, err;
	enum nl80211_band band;

	if (n_ssids < 0 || n_freqs < 0)
		return -EINVAL;
	if (!wdev)
		return -ENODEV;

	rdev = wiphy_to_rdev(wdev->wiphy);

	guard(wiphy)(wdev->wiphy);

	/* The checks nl80211 makes before it will submit a scan. */
	if (!rdev->ops->scan)
		return -EOPNOTSUPP;
	if (!wdev_running(wdev))
		return -ENETDOWN;
	if (n_ssids > wdev->wiphy->max_scan_ssids)
		return -EINVAL;
	if (ie_len > wdev->wiphy->max_scan_ie_len)
		return -EINVAL;
	for (i = 0; i < n_ssids; i++)
		if (ssids[i].ssid_len > IEEE80211_MAX_SSID_LEN)
			return -EINVAL;

	if (rdev->scan_req || rdev->scan_msg)
		return -EBUSY;

	n_channels = n_freqs ? n_freqs :
		     ieee80211_get_num_supported_channels(wdev->wiphy);
	if (!n_channels)
		return -EINVAL;

	request = kzalloc(struct_size(request, req.channels, n_channels) +
			  sizeof(request->req.ssids[0]) * n_ssids + ie_len,
			  GFP_KERNEL);
	if (!request)
		return -ENOMEM;

	i = 0;
	if (n_freqs) {
		for (j = 0; j < n_freqs; j++) {
			struct ieee80211_channel *chan;

			chan = ieee80211_get_channel(wdev->wiphy, freqs[j]);
			if (!chan || chan->flags & IEEE80211_CHAN_DISABLED)
				continue;
			request->req.channels[i++] = chan;
			request->req.rates[chan->band] =
				(1 << wdev->wiphy->bands[chan->band]->n_bitrates) - 1;
		}
	} else {
		for (band = 0; band < NUM_NL80211_BANDS; band++) {
			struct ieee80211_supported_band *sband;

			sband = wdev->wiphy->bands[band];
			if (!sband)
				continue;
			for (j = 0; j < sband->n_channels; j++) {
				if (sband->channels[j].flags &
				    IEEE80211_CHAN_DISABLED)
					continue;
				request->req.channels[i++] = &sband->channels[j];
			}
			request->req.rates[band] = (1 << sband->n_bitrates) - 1;
		}
	}

	if (!i) {
		kfree(request);
		return -EINVAL;
	}
	request->req.n_channels = i;

	if (n_ssids) {
		request->req.ssids = (void *)request +
			struct_size(request, req.channels, n_channels);
		request->req.n_ssids = n_ssids;
		memcpy(request->req.ssids, ssids,
		       sizeof(*ssids) * n_ssids);
	}

	if (ie_len) {
		u8 *iebuf = (void *)request +
			struct_size(request, req.channels, n_channels) +
			sizeof(request->req.ssids[0]) * n_ssids;

		memcpy(iebuf, ie, ie_len);
		request->req.ie = iebuf;
		request->req.ie_len = ie_len;
	}

	eth_broadcast_addr(request->req.bssid);
	request->req.wdev = wdev;
	request->req.wiphy = &rdev->wiphy;
	request->req.scan_start = jiffies;
	request->req.tsf_report_link_id = -1;

	rdev->scan_req = request;

	/*
	 * Not rdev_scan(): cfg80211_scan() sets first_part and performs
	 * the 6 GHz split that WIPHY_FLAG_SPLIT_SCAN_6GHZ drivers
	 * require.
	 */
	err = cfg80211_scan(rdev);
	if (err) {
		rdev->scan_req = NULL;
		kfree(request);
		return err;
	}

	nl80211_send_scan_start(rdev, wdev);
	dev_hold(wdev->netdev);

	return 0;
}
EXPORT_SYMBOL_GPL(cfg80211_ik_scan);

int cfg80211_ik_abort_scan(struct cfg80211_ik *ik)
{
	struct wireless_dev *wdev = ik_wdev(ik);
	struct cfg80211_registered_device *rdev;

	if (!wdev)
		return -ENODEV;
	rdev = wiphy_to_rdev(wdev->wiphy);

	guard(wiphy)(wdev->wiphy);

	if (!rdev->ops->abort_scan)
		return -EOPNOTSUPP;
	if (!rdev->scan_req)
		return 0;

	rdev_abort_scan(rdev, wdev);
	return 0;
}
EXPORT_SYMBOL_GPL(cfg80211_ik_abort_scan);

void cfg80211_ik_bss_iter(struct cfg80211_ik *ik,
			  void (*iter)(struct wiphy *wiphy,
				       struct cfg80211_bss *bss, void *data),
			  void *data)
{
	struct wireless_dev *wdev = ik_wdev(ik);

	if (!wdev)
		return;

	cfg80211_bss_iter(wdev->wiphy, NULL, iter, data);
}
EXPORT_SYMBOL_GPL(cfg80211_ik_bss_iter);

int cfg80211_ik_connect(struct cfg80211_ik *ik,
			struct cfg80211_connect_params *sme)
{
	struct wireless_dev *wdev = ik_wdev(ik);
	struct cfg80211_registered_device *rdev;

	if (sme->ssid_len > IEEE80211_MAX_SSID_LEN)
		return -EINVAL;
	if (!wdev)
		return -ENODEV;
	rdev = wiphy_to_rdev(wdev->wiphy);

	guard(wiphy)(wdev->wiphy);

	if (!rdev->ops->connect && !rdev->ops->auth)
		return -EOPNOTSUPP;
	if (!wdev_running(wdev))
		return -ENETDOWN;

	return cfg80211_connect(rdev, wdev->netdev, sme, NULL, NULL);
}
EXPORT_SYMBOL_GPL(cfg80211_ik_connect);

int cfg80211_ik_disconnect(struct cfg80211_ik *ik, u16 reason)
{
	struct wireless_dev *wdev = ik_wdev(ik);
	struct cfg80211_registered_device *rdev;

	if (!wdev)
		return -ENODEV;
	rdev = wiphy_to_rdev(wdev->wiphy);

	guard(wiphy)(wdev->wiphy);

	return cfg80211_disconnect(rdev, wdev->netdev, reason, true);
}
EXPORT_SYMBOL_GPL(cfg80211_ik_disconnect);

int cfg80211_ik_add_key(struct cfg80211_ik *ik, u8 key_index, bool pairwise,
			const u8 *mac_addr, struct key_params *params)
{
	struct wireless_dev *wdev = ik_wdev(ik);
	struct cfg80211_registered_device *rdev;
	int err;

	if (!wdev)
		return -ENODEV;
	rdev = wiphy_to_rdev(wdev->wiphy);

	guard(wiphy)(wdev->wiphy);

	if (!rdev->ops->add_key)
		return -EOPNOTSUPP;

	/*
	 * nl80211 validates through its netlink policy and this helper;
	 * with no userspace peer this API is the boundary, and without it
	 * a short TKIP key makes mac80211 read past the key material.
	 */
	err = cfg80211_validate_key_settings(rdev, wdev, params, key_index,
					     pairwise, mac_addr);
	if (err)
		return err;

	return rdev_add_key(rdev, wdev, -1, key_index, pairwise, mac_addr,
			    params);
}
EXPORT_SYMBOL_GPL(cfg80211_ik_add_key);

int cfg80211_ik_del_key(struct cfg80211_ik *ik, u8 key_index, bool pairwise,
			const u8 *mac_addr)
{
	struct wireless_dev *wdev = ik_wdev(ik);
	struct cfg80211_registered_device *rdev;

	if (!wdev)
		return -ENODEV;
	rdev = wiphy_to_rdev(wdev->wiphy);

	guard(wiphy)(wdev->wiphy);

	if (!rdev->ops->del_key)
		return -EOPNOTSUPP;

	return rdev_del_key(rdev, wdev, -1, key_index, pairwise, mac_addr);
}
EXPORT_SYMBOL_GPL(cfg80211_ik_del_key);

int cfg80211_ik_set_default_key(struct cfg80211_ik *ik, u8 key_index,
				bool unicast, bool multicast)
{
	struct wireless_dev *wdev = ik_wdev(ik);
	struct cfg80211_registered_device *rdev;

	if (!wdev)
		return -ENODEV;
	rdev = wiphy_to_rdev(wdev->wiphy);

	guard(wiphy)(wdev->wiphy);

	if (!rdev->ops->set_default_key)
		return -EOPNOTSUPP;

	return rdev_set_default_key(rdev, wdev->netdev, -1, key_index,
				    unicast, multicast);
}
EXPORT_SYMBOL_GPL(cfg80211_ik_set_default_key);

int cfg80211_ik_set_default_mgmt_key(struct cfg80211_ik *ik, u8 key_index)
{
	struct wireless_dev *wdev = ik_wdev(ik);
	struct cfg80211_registered_device *rdev;

	if (!wdev)
		return -ENODEV;
	rdev = wiphy_to_rdev(wdev->wiphy);

	guard(wiphy)(wdev->wiphy);

	if (!rdev->ops->set_default_mgmt_key)
		return -EOPNOTSUPP;

	return rdev_set_default_mgmt_key(rdev, wdev, -1, key_index);
}
EXPORT_SYMBOL_GPL(cfg80211_ik_set_default_mgmt_key);

int cfg80211_ik_set_authorized(struct cfg80211_ik *ik, const u8 *mac,
			       bool authorized)
{
	struct wireless_dev *wdev = ik_wdev(ik);
	struct cfg80211_registered_device *rdev;
	struct station_parameters params = {};

	if (!wdev)
		return -ENODEV;
	rdev = wiphy_to_rdev(wdev->wiphy);

	guard(wiphy)(wdev->wiphy);

	if (!rdev->ops->change_station)
		return -EOPNOTSUPP;

	/*
	 * These use -1, not 0, to mean "not specified"; a zeroed struct
	 * reads as an explicit request and is rejected.
	 */
	params.listen_interval = -1;
	params.support_p2p_ps = -1;

	params.sta_flags_mask = BIT(NL80211_STA_FLAG_AUTHORIZED);
	if (authorized)
		params.sta_flags_set = BIT(NL80211_STA_FLAG_AUTHORIZED);
	params.link_sta_params.link_id = -1;

	return rdev_change_station(rdev, wdev, (u8 *)mac, &params);
}
EXPORT_SYMBOL_GPL(cfg80211_ik_set_authorized);

int cfg80211_ik_get_station(struct cfg80211_ik *ik, const u8 *mac,
			    struct station_info *sinfo)
{
	struct wireless_dev *wdev = ik_wdev(ik);
	struct cfg80211_registered_device *rdev;
	int err;

	if (!wdev)
		return -ENODEV;
	rdev = wiphy_to_rdev(wdev->wiphy);

	guard(wiphy)(wdev->wiphy);

	if (!rdev->ops->get_station)
		return -EOPNOTSUPP;

	memset(sinfo, 0, sizeof(*sinfo));

	err = rdev_get_station(rdev, wdev, mac, sinfo);
	if (err)
		cfg80211_sinfo_release_content(sinfo);

	return err;
}
EXPORT_SYMBOL_GPL(cfg80211_ik_get_station);

/*
 * Notification hooks, called from the cfg80211 entry points drivers use.
 * They run in the driver's context, which may be atomic, so they only
 * forward and never block.
 */

void cfg80211_ik_notify_scan_done(struct wiphy *wiphy, bool aborted)
{
	struct cfg80211_ik *ik;

	guard(rcu)();

	ik = rcu_dereference(ik_consumer);
	if (!ik || !ik_wdev(ik) || ik_wdev(ik)->wiphy != wiphy)
		return;
	if (ik->ops->scan_done)
		ik->ops->scan_done(ik->priv, aborted);
}

void cfg80211_ik_notify_connect(struct net_device *dev,
				struct cfg80211_connect_resp_params *params)
{
	struct cfg80211_ik *ik;

	guard(rcu)();

	ik = rcu_dereference(ik_consumer);
	if (!ik || !ik_wdev(ik) || ik_wdev(ik)->netdev != dev)
		return;
	if (ik->ops->connect_result)
		ik->ops->connect_result(ik->priv, params->links[0].bssid,
					params->status,
					params->req_ie, params->req_ie_len,
					params->resp_ie, params->resp_ie_len);
}

void cfg80211_ik_notify_disconnect(struct net_device *dev, u16 reason,
				   const u8 *ie, size_t ie_len,
				   bool locally_generated)
{
	struct cfg80211_ik *ik;

	guard(rcu)();

	ik = rcu_dereference(ik_consumer);
	if (!ik || !ik_wdev(ik) || ik_wdev(ik)->netdev != dev)
		return;
	if (ik->ops->disconnected)
		ik->ops->disconnected(ik->priv, reason, locally_generated,
				      ie, ie_len);
}
