// SPDX-License-Identifier: GPL-2.0-only
/*
 * The in-kernel client API of cfg80211. A client drives one station
 * interface as wpa_supplicant does through nl80211, and its calls run the
 * checks of the matching nl80211 commands.
 *
 * The interface's client pointer is written with the RTNL and the wiphy lock
 * held. The notifications read it under the lock that their caller holds,
 * or under RCU in atomic context.
 */

#include <linux/etherdevice.h>
#include <linux/rtnetlink.h>
#include <linux/slab.h>
#include <net/cfg80211.h>
#include "core.h"
#include "nl80211.h"
#include "rdev-ops.h"
#include "reg.h"

struct cfg80211_client {
	struct cfg80211_registered_device *rdev;
	struct wireless_dev *wdev;
	const struct cfg80211_client_ops *ops;
	void *priv;
	/* Set with the RTNL and the wiphy lock held. */
	bool gone;
	/*
	 * The client's running scan, and the jiffies when its last scan
	 * started or when it registered.
	 */
	struct cfg80211_scan_request_int *scan_req;
	unsigned long scan_start;
};

struct cfg80211_client *
cfg80211_client_register(struct net_device *dev,
			 const struct cfg80211_client_ops *ops, void *priv)
{
	struct wireless_dev *wdev = dev->ieee80211_ptr;
	struct cfg80211_registered_device *rdev;
	struct cfg80211_client *client;
	int err = 0;

	if (!wdev)
		return ERR_PTR(-EINVAL);
	rdev = wiphy_to_rdev(wdev->wiphy);

	client = kzalloc_obj(*client);
	if (!client)
		return ERR_PTR(-ENOMEM);
	client->rdev = rdev;
	client->wdev = wdev;
	client->ops = ops;
	client->priv = priv;
	client->scan_start = jiffies;

	rtnl_lock();
	wiphy_lock(&rdev->wiphy);
	if (!wdev->registered) {
		err = -ENODEV;
	} else if (wdev->iftype != NL80211_IFTYPE_STATION) {
		err = -EOPNOTSUPP;
	} else if (rcu_access_pointer(wdev->client)) {
		err = -EBUSY;
	} else {
		get_device(&rdev->wiphy.dev);
		rcu_assign_pointer(wdev->client, client);
	}
	wiphy_unlock(&rdev->wiphy);
	rtnl_unlock();

	if (err) {
		kfree(client);
		return ERR_PTR(err);
	}
	return client;
}
EXPORT_SYMBOL_GPL(cfg80211_client_register);

void cfg80211_client_unregister(struct cfg80211_client *client)
{
	struct cfg80211_registered_device *rdev = client->rdev;

	rtnl_lock();
	wiphy_lock(&rdev->wiphy);
	if (!client->gone)
		RCU_INIT_POINTER(client->wdev->client, NULL);
	wiphy_unlock(&rdev->wiphy);
	rtnl_unlock();

	synchronize_rcu();
	put_device(&rdev->wiphy.dev);
	kfree(client);
}
EXPORT_SYMBOL_GPL(cfg80211_client_unregister);

/*
 * Take the wiphy lock for a call, which fails while the interface is down
 * or after it is gone.
 */
static int client_lock(struct cfg80211_client *client)
{
	wiphy_lock(&client->rdev->wiphy);
	if (client->gone) {
		wiphy_unlock(&client->rdev->wiphy);
		return -ENODEV;
	}
	if (!wdev_running(client->wdev)) {
		wiphy_unlock(&client->rdev->wiphy);
		return -ENETDOWN;
	}
	return 0;
}

static void client_unlock(struct cfg80211_client *client)
{
	wiphy_unlock(&client->rdev->wiphy);
}

int cfg80211_client_open(struct cfg80211_client *client)
{
	int err;

	rtnl_lock();
	if (client->gone)
		err = -ENODEV;
	else
		err = dev_open(client->wdev->netdev, NULL);
	rtnl_unlock();
	return err;
}
EXPORT_SYMBOL_GPL(cfg80211_client_open);

int cfg80211_client_close(struct cfg80211_client *client)
{
	int err = 0;

	rtnl_lock();
	if (client->gone)
		err = -ENODEV;
	else if (!netif_running(client->wdev->netdev))
		err = -ENETDOWN;
	else
		dev_close(client->wdev->netdev);
	rtnl_unlock();
	return err;
}
EXPORT_SYMBOL_GPL(cfg80211_client_close);

int cfg80211_client_set_mac(struct cfg80211_client *client, const u8 *addr)
{
	struct sockaddr_storage ss = {};
	struct net_device *dev;
	int err;

	if (!is_valid_ether_addr(addr))
		return -EINVAL;

	rtnl_lock();
	if (client->gone) {
		err = -ENODEV;
	} else {
		dev = client->wdev->netdev;
		ss.ss_family = dev->type;
		memcpy(ss.__data, addr, ETH_ALEN);
		err = dev_set_mac_address_user(dev, &ss, NULL);
	}
	rtnl_unlock();
	return err;
}
EXPORT_SYMBOL_GPL(cfg80211_client_set_mac);

int cfg80211_client_set_country(struct cfg80211_client *client,
				const char *alpha2)
{
	int err;

	rtnl_lock();
	if (client->gone)
		err = -ENODEV;
	else if (!rcu_access_pointer(cfg80211_regdomain))
		err = -EINPROGRESS;
	else
		err = regulatory_hint_user(alpha2, NL80211_USER_REG_HINT_USER);
	rtnl_unlock();
	return err;
}
EXPORT_SYMBOL_GPL(cfg80211_client_set_country);

int cfg80211_client_get_country(struct cfg80211_client *client, char *alpha2)
{
	const struct ieee80211_regdomain *regd;
	int err = 0;

	rcu_read_lock();
	regd = get_wiphy_regdom(&client->rdev->wiphy);
	if (!regd)
		regd = rcu_dereference(cfg80211_regdomain);
	if (regd)
		memcpy(alpha2, regd->alpha2, 2);
	else
		err = -ENOENT;
	rcu_read_unlock();
	return err;
}
EXPORT_SYMBOL_GPL(cfg80211_client_get_country);

static int client_check_scan(struct wiphy *wiphy,
			     const struct cfg80211_client_scan_params *params)
{
	int i, j;

	if (params->n_ssids < 0 || params->n_ssids > wiphy->max_scan_ssids ||
	    (params->n_ssids && !params->ssids))
		return -EINVAL;

	for (i = 0; i < params->n_ssids; i++)
		if (params->ssids[i].ssid_len > IEEE80211_MAX_SSID_LEN)
			return -EINVAL;

	if (params->ie_len > wiphy->max_scan_ie_len ||
	    params->ie_len > IEEE80211_MAX_DATA_LEN ||
	    (params->ie_len && !params->ie) ||
	    !nl80211_valid_ies(params->ie, params->ie_len))
		return -EINVAL;

	if (params->freqs && params->n_freqs <= 0)
		return -EINVAL;

	/* Scanning a channel twice is refused, as nl80211 does. */
	for (i = 0; params->freqs && i < params->n_freqs; i++)
		for (j = i + 1; j < params->n_freqs; j++)
			if (params->freqs[i] == params->freqs[j])
				return -EINVAL;

	return 0;
}

int cfg80211_client_scan(struct cfg80211_client *client,
			 const struct cfg80211_client_scan_params *params)
{
	struct cfg80211_registered_device *rdev = client->rdev;
	struct wireless_dev *wdev = client->wdev;
	struct wiphy *wiphy = &rdev->wiphy;
	struct cfg80211_scan_request_int *request;
	size_t size, ssids_offset, ie_offset;
	int err, i, n_channels;

	err = client_lock(client);
	if (err)
		return err;

	if (!rdev->ops->scan) {
		err = -EOPNOTSUPP;
		goto out;
	}

	if (rdev->scan_req || rdev->scan_msg) {
		err = -EBUSY;
		goto out;
	}

	err = client_check_scan(wiphy, params);
	if (err)
		goto out;

	err = nl80211_check_scan_feat_flags(wiphy, wdev, params->flags,
					    NL80211_FEATURE_SCAN_RANDOM_MAC_ADDR);
	if (err)
		goto out;

	if (params->freqs)
		n_channels = params->n_freqs;
	else
		n_channels = ieee80211_get_num_supported_channels(wiphy);

	size = struct_size(request, req.channels, n_channels);
	ssids_offset = size;
	size = size_add(size, array_size(sizeof(*request->req.ssids),
					 params->n_ssids));
	ie_offset = size;
	size = size_add(size, params->ie_len);
	request = kzalloc(size, GFP_KERNEL);
	if (!request) {
		err = -ENOMEM;
		goto out;
	}

	if (params->n_ssids) {
		request->req.ssids = (void *)request + ssids_offset;
		memcpy(request->req.ssids, params->ssids,
		       params->n_ssids * sizeof(*params->ssids));
	}
	request->req.n_ssids = params->n_ssids;

	if (params->ie_len) {
		request->req.ie = (void *)request + ie_offset;
		memcpy((void *)request->req.ie, params->ie, params->ie_len);
	}
	request->req.ie_len = params->ie_len;

	if (params->freqs) {
		n_channels = 0;
		for (i = 0; i < params->n_freqs; i++) {
			struct ieee80211_channel *chan;

			chan = ieee80211_get_channel_khz(wiphy,
							 MHZ_TO_KHZ(params->freqs[i]));
			if (!chan) {
				err = -EINVAL;
				goto out_free;
			}
			if (nl80211_scan_chan_usable(wdev, chan))
				request->req.channels[n_channels++] = chan;
		}
	} else {
		n_channels = nl80211_scan_all_chans(wdev, request->req.channels);
	}

	if (!n_channels) {
		err = -EINVAL;
		goto out_free;
	}
	request->req.n_channels = n_channels;

	err = nl80211_check_scan_chans(wdev, &request->req);
	if (err)
		goto out_free;

	for (i = 0; i < NUM_NL80211_BANDS; i++)
		if (wiphy->bands[i])
			request->req.rates[i] =
				(1 << wiphy->bands[i]->n_bitrates) - 1;

	request->req.flags = params->flags;
	if (params->flags & NL80211_SCAN_FLAG_RANDOM_ADDR) {
		/*
		 * An all-zero mask is invalid, and selects the address that
		 * nl80211 uses when none is given.
		 */
		if (is_zero_ether_addr(params->mac_addr_mask)) {
			request->req.mac_addr[0] = 0x2;
			request->req.mac_addr_mask[0] = 0x3;
		} else {
			ether_addr_copy(request->req.mac_addr, params->mac_addr);
			ether_addr_copy(request->req.mac_addr_mask,
					params->mac_addr_mask);
			err = nl80211_check_random_mac(request->req.mac_addr,
						       request->req.mac_addr_mask);
			if (err)
				goto out_free;
		}
	}

	if (params->bssid)
		ether_addr_copy(request->req.bssid, params->bssid);
	else
		eth_broadcast_addr(request->req.bssid);

	request->req.tsf_report_link_id = -1;
	request->req.wdev = wdev;
	request->req.wiphy = wiphy;
	request->req.scan_start = jiffies;

	rdev->scan_req = request;
	err = cfg80211_scan(rdev);
	if (err)
		goto out_free;

	nl80211_send_scan_start(rdev, wdev);
	dev_hold(wdev->netdev);
	client->scan_req = request;
	client->scan_start = request->req.scan_start;
	goto out;

out_free:
	rdev->scan_req = NULL;
	kfree(request);
out:
	client_unlock(client);
	return err;
}
EXPORT_SYMBOL_GPL(cfg80211_client_scan);

int cfg80211_client_abort_scan(struct cfg80211_client *client)
{
	struct cfg80211_registered_device *rdev = client->rdev;
	int err;

	err = client_lock(client);
	if (err)
		return err;

	if (!rdev->ops->abort_scan)
		err = -EOPNOTSUPP;
	else if (!client->scan_req || rdev->scan_req != client->scan_req)
		err = -ENOENT;
	else
		rdev_abort_scan(rdev, client->wdev);

	client_unlock(client);
	return err;
}
EXPORT_SYMBOL_GPL(cfg80211_client_abort_scan);

int cfg80211_client_get_scan_bss(struct cfg80211_client *client,
				 struct cfg80211_bss ***bss)
{
	int ret;

	ret = client_lock(client);
	if (ret)
		return ret;

	ret = cfg80211_bss_refs_since(client->rdev, client->scan_start, bss);

	client_unlock(client);
	return ret;
}
EXPORT_SYMBOL_GPL(cfg80211_client_get_scan_bss);

static bool client_valid_chan(struct wiphy *wiphy,
			      struct ieee80211_channel *chan)
{
	return nl80211_get_valid_chan(wiphy, ieee80211_channel_to_khz(chan)) ==
	       chan;
}

/* cfg80211's SME takes a static WEP key only as the default connect key. */
static struct cfg80211_cached_keys *
client_connkeys(struct cfg80211_client *client,
		struct cfg80211_connect_params *connect)
{
	struct key_params params = {
		.key = connect->key,
		.key_len = connect->key_len,
	};
	struct cfg80211_cached_keys *keys;
	int idx = connect->key_idx;
	int err;

	if (!connect->privacy || !connect->key || idx < 0 || idx > 3)
		return ERR_PTR(-EINVAL);

	switch (connect->key_len) {
	case WLAN_KEY_LEN_WEP40:
		params.cipher = WLAN_CIPHER_SUITE_WEP40;
		break;
	case WLAN_KEY_LEN_WEP104:
		params.cipher = WLAN_CIPHER_SUITE_WEP104;
		break;
	default:
		return ERR_PTR(-EINVAL);
	}

	err = cfg80211_validate_key_settings(client->rdev, client->wdev,
					     &params, idx, false, NULL);
	if (err)
		return ERR_PTR(err);

	keys = kzalloc_obj(*keys);
	if (!keys)
		return ERR_PTR(-ENOMEM);

	keys->def = idx;
	keys->params[idx].cipher = params.cipher;
	keys->params[idx].key_len = params.key_len;
	keys->params[idx].key = keys->data[idx];
	memcpy(keys->data[idx], params.key, params.key_len);

	connect->key = NULL;
	connect->key_len = 0;
	connect->key_idx = 0;
	return keys;
}

static int client_check_connect(struct cfg80211_registered_device *rdev,
				struct cfg80211_connect_params *connect)
{
	struct wiphy *wiphy = &rdev->wiphy;
	int err;
	u32 flags = ASSOC_REQ_DISABLE_HT | ASSOC_REQ_DISABLE_VHT |
		    ASSOC_REQ_DISABLE_HE | ASSOC_REQ_DISABLE_EHT |
		    ASSOC_REQ_DISABLE_UHR | ASSOC_REQ_USE_RRM;

	if (!connect->ssid || !connect->ssid_len ||
	    connect->ssid_len > IEEE80211_MAX_SSID_LEN)
		return -EINVAL;

	if (connect->ie_len > IEEE80211_MAX_DATA_LEN ||
	    (connect->ie_len && !connect->ie) ||
	    !nl80211_valid_ies(connect->ie, connect->ie_len))
		return -EINVAL;

	if (connect->crypto.wpa_versions & ~NL80211_WPA_VERSIONS_VALID)
		return -EINVAL;

	if (connect->auth_type != NL80211_AUTHTYPE_AUTOMATIC &&
	    !nl80211_valid_auth_type(rdev, connect->auth_type,
				     NL80211_CMD_CONNECT))
		return -EINVAL;

	if (connect->want_1x &&
	    !wiphy_ext_feature_isset(wiphy,
				     NL80211_EXT_FEATURE_4WAY_HANDSHAKE_STA_1X))
		return -EINVAL;

	/* Delivering EAPOL over nl80211 needs an owning socket. */
	if (connect->crypto.control_port_over_nl80211)
		return -EINVAL;

	if (!connect->crypto.control_port_ethertype)
		connect->crypto.control_port_ethertype = cpu_to_be16(ETH_P_PAE);

	err = nl80211_check_crypto_settings(rdev, &connect->crypto,
					    NL80211_MAX_NR_CIPHER_SUITES);
	if (err)
		return err;

	if (connect->mfp > NL80211_MFP_OPTIONAL)
		return -EINVAL;
	if (connect->mfp == NL80211_MFP_OPTIONAL &&
	    !wiphy_ext_feature_isset(wiphy, NL80211_EXT_FEATURE_MFP_OPTIONAL))
		return -EOPNOTSUPP;

	if ((connect->channel && !client_valid_chan(wiphy, connect->channel)) ||
	    (connect->channel_hint &&
	     !client_valid_chan(wiphy, connect->channel_hint)))
		return -EINVAL;

	if (connect->flags & ~flags)
		return -EINVAL;

	if ((connect->flags & ASSOC_REQ_USE_RRM) &&
	    !((wiphy->features & NL80211_FEATURE_DS_PARAM_SET_IE_IN_PROBES) &&
	      (wiphy->features & NL80211_FEATURE_QUIET)) &&
	    !wiphy_ext_feature_isset(wiphy, NL80211_EXT_FEATURE_RRM))
		return -EINVAL;

	if (connect->pbss || connect->bss_select.behaviour ||
	    connect->fils_erp_username || connect->fils_erp_realm ||
	    connect->fils_erp_rrk)
		return -EINVAL;

	return 0;
}

int cfg80211_client_connect(struct cfg80211_client *client,
			    const struct cfg80211_connect_params *params)
{
	struct cfg80211_registered_device *rdev = client->rdev;
	struct cfg80211_connect_params connect_copy = *params;
	struct cfg80211_connect_params *connect = &connect_copy;
	struct cfg80211_cached_keys *connkeys = NULL;
	int err;

	err = client_lock(client);
	if (err)
		return err;

	if (client->wdev->iftype != NL80211_IFTYPE_STATION) {
		err = -EOPNOTSUPP;
		goto out;
	}

	err = client_check_connect(rdev, connect);
	if (err)
		goto out;

	if (connect->key_len) {
		connkeys = client_connkeys(client, connect);
		if (IS_ERR(connkeys)) {
			err = PTR_ERR(connkeys);
			goto out;
		}
	}

	err = cfg80211_connect(rdev, client->wdev->netdev, connect, connkeys,
			       connect->prev_bssid);
	if (err)
		kfree_sensitive(connkeys);
out:
	client_unlock(client);
	return err;
}
EXPORT_SYMBOL_GPL(cfg80211_client_connect);

int cfg80211_client_disconnect(struct cfg80211_client *client, u16 reason)
{
	struct wireless_dev *wdev = client->wdev;
	int err;

	if (!reason)
		return -EINVAL;

	err = client_lock(client);
	if (err)
		return err;

	if (wdev->iftype != NL80211_IFTYPE_STATION)
		err = -EOPNOTSUPP;
	else if (wdev->conn_owner_nlportid)
		err = -EPERM;
	else
		err = cfg80211_disconnect(client->rdev, wdev->netdev, reason,
					  true);

	client_unlock(client);
	return err;
}
EXPORT_SYMBOL_GPL(cfg80211_client_disconnect);

int cfg80211_client_add_key(struct cfg80211_client *client, u8 key_idx,
			    bool pairwise, const u8 *mac_addr,
			    struct key_params *params)
{
	struct cfg80211_registered_device *rdev = client->rdev;
	struct wireless_dev *wdev = client->wdev;
	int err;

	err = client_lock(client);
	if (err)
		return err;

	if (!params->key || params->key_len > WLAN_MAX_KEY_LEN ||
	    params->seq_len > NL80211_KEY_SEQ_MAX_LEN ||
	    (params->seq_len && !params->seq) ||
	    params->mode > NL80211_KEY_SET_TX)
		err = -EINVAL;
	else if (!rdev->ops->add_key)
		err = -EOPNOTSUPP;
	else if (cfg80211_validate_key_settings(rdev, wdev, params, key_idx,
						pairwise, mac_addr))
		err = -EINVAL;
	else
		err = nl80211_key_allowed(wdev);

	/* A connection that the client starts has no MLO links. */
	if (!err && wdev->valid_links)
		err = -EINVAL;

	if (!err)
		err = rdev_add_key(rdev, wdev, -1, key_idx, pairwise, mac_addr,
				   params);

	client_unlock(client);
	return err;
}
EXPORT_SYMBOL_GPL(cfg80211_client_add_key);

int cfg80211_client_del_key(struct cfg80211_client *client, u8 key_idx,
			    bool pairwise, const u8 *mac_addr)
{
	struct cfg80211_registered_device *rdev = client->rdev;
	struct wireless_dev *wdev = client->wdev;
	int err;

	err = client_lock(client);
	if (err)
		return err;

	if (!cfg80211_valid_key_idx(rdev, key_idx, pairwise)) {
		err = -EINVAL;
	} else if (!rdev->ops->del_key) {
		err = -EOPNOTSUPP;
	} else {
		err = nl80211_key_allowed(wdev);
		if (!pairwise && mac_addr &&
		    !(rdev->wiphy.flags & WIPHY_FLAG_IBSS_RSN))
			err = -ENOENT;
	}

	if (!err && wdev->valid_links)
		err = -EINVAL;

	if (!err)
		err = rdev_del_key(rdev, wdev, -1, key_idx, pairwise, mac_addr);

	client_unlock(client);
	return err;
}
EXPORT_SYMBOL_GPL(cfg80211_client_del_key);

int cfg80211_client_set_default_key(struct cfg80211_client *client,
				    u8 key_idx, bool unicast, bool multicast)
{
	struct cfg80211_registered_device *rdev = client->rdev;
	struct wireless_dev *wdev = client->wdev;
	int err;

	err = client_lock(client);
	if (err)
		return err;

	if (key_idx > 3)
		err = -EINVAL;
	else if (!rdev->ops->set_default_key)
		err = -EOPNOTSUPP;
	else
		err = nl80211_key_allowed(wdev);

	if (!err && wdev->valid_links)
		err = -EINVAL;

	if (!err)
		err = rdev_set_default_key(rdev, wdev->netdev, -1, key_idx,
					   unicast, multicast);

	client_unlock(client);
	return err;
}
EXPORT_SYMBOL_GPL(cfg80211_client_set_default_key);

int cfg80211_client_set_default_mgmt_key(struct cfg80211_client *client,
					 u8 key_idx)
{
	struct cfg80211_registered_device *rdev = client->rdev;
	struct wireless_dev *wdev = client->wdev;
	int err;

	err = client_lock(client);
	if (err)
		return err;

	if (key_idx < 4 || key_idx > 5)
		err = -EINVAL;
	else if (!rdev->ops->set_default_mgmt_key)
		err = -EOPNOTSUPP;
	else
		err = nl80211_key_allowed(wdev);

	if (!err && wdev->valid_links)
		err = -EINVAL;

	if (!err)
		err = rdev_set_default_mgmt_key(rdev, wdev, -1, key_idx);

	client_unlock(client);
	return err;
}
EXPORT_SYMBOL_GPL(cfg80211_client_set_default_mgmt_key);

int cfg80211_client_authorize(struct cfg80211_client *client,
			      const u8 *mac_addr, bool authorized)
{
	struct cfg80211_registered_device *rdev = client->rdev;
	struct station_parameters params = {
		.sta_flags_mask = BIT(NL80211_STA_FLAG_AUTHORIZED),
		.sta_flags_set = authorized ? BIT(NL80211_STA_FLAG_AUTHORIZED) : 0,
		.listen_interval = -1,
		.support_p2p_ps = -1,
		.link_sta_params.link_id = -1,
	};
	int err;

	err = client_lock(client);
	if (err)
		return err;

	if (!rdev->ops->change_station ||
	    client->wdev->iftype != NL80211_IFTYPE_STATION)
		err = -EOPNOTSUPP;
	else
		err = rdev_change_station(rdev, client->wdev, (u8 *)mac_addr,
					  &params);

	client_unlock(client);
	return err;
}
EXPORT_SYMBOL_GPL(cfg80211_client_authorize);

int cfg80211_client_get_station(struct cfg80211_client *client,
				const u8 *mac_addr, struct station_info *sinfo)
{
	struct cfg80211_registered_device *rdev = client->rdev;
	int err, i;

	memset(sinfo, 0, sizeof(*sinfo));

	err = client_lock(client);
	if (err)
		return err;

	if (!rdev->ops->get_station) {
		err = -EOPNOTSUPP;
		goto out;
	}

	for (i = 0; i < IEEE80211_MLD_MAX_NUM_LINKS; i++) {
		sinfo->links[i] = kzalloc_obj(*sinfo->links[0]);
		if (!sinfo->links[i]) {
			err = -ENOMEM;
			goto out_release;
		}
	}

	err = rdev_get_station(rdev, client->wdev, mac_addr, sinfo);
	if (err)
		goto out_release;

	if (sinfo->valid_links)
		cfg80211_sta_set_mld_sinfo(sinfo);
	goto out;

out_release:
	cfg80211_sinfo_release_content(sinfo);
out:
	client_unlock(client);
	return err;
}
EXPORT_SYMBOL_GPL(cfg80211_client_get_station);

int cfg80211_client_set_power_mgmt(struct cfg80211_client *client,
				   bool enabled, int timeout)
{
	struct cfg80211_registered_device *rdev = client->rdev;
	struct wireless_dev *wdev = client->wdev;
	int err;

	err = client_lock(client);
	if (err)
		return err;

	if (!rdev->ops->set_power_mgmt) {
		err = -EOPNOTSUPP;
	} else if (enabled != wdev->ps || timeout != wdev->ps_timeout) {
		err = rdev_set_power_mgmt(rdev, wdev->netdev, enabled, timeout);
		if (!err) {
			wdev->ps = enabled;
			wdev->ps_timeout = timeout;
		}
	}

	client_unlock(client);
	return err;
}
EXPORT_SYMBOL_GPL(cfg80211_client_set_power_mgmt);

void cfg80211_client_scan_done(struct cfg80211_scan_request_int *req,
			       bool aborted)
{
	struct wireless_dev *wdev = req->req.wdev;
	struct cfg80211_client *client;

	client = wiphy_dereference(wdev->wiphy, wdev->client);
	if (!client || client->scan_req != req)
		return;

	client->scan_req = NULL;
	if (client->ops->scan_done)
		client->ops->scan_done(client->priv, aborted);
}

void cfg80211_client_connect_result(struct wireless_dev *wdev,
				    const struct cfg80211_connect_resp_params *cr)
{
	struct cfg80211_client *client;

	client = wiphy_dereference(wdev->wiphy, wdev->client);
	if (client && client->ops->connect_result)
		client->ops->connect_result(client->priv, cr);
}

void cfg80211_client_disconnected(struct wireless_dev *wdev, u16 reason,
				  const u8 *ie, size_t ie_len, bool from_ap)
{
	struct cfg80211_client *client;

	client = wiphy_dereference(wdev->wiphy, wdev->client);
	if (client && client->ops->disconnected)
		client->ops->disconnected(client->priv, reason, ie, ie_len,
					  !from_ap);
}

void cfg80211_client_mic_failure(struct wireless_dev *wdev, const u8 *addr,
				 enum nl80211_key_type key_type, int key_id,
				 const u8 *tsc)
{
	struct cfg80211_client *client;

	rcu_read_lock();
	client = rcu_dereference(wdev->client);
	if (client && client->ops->mic_failure)
		client->ops->mic_failure(client->priv, addr, key_type, key_id,
					 tsc);
	rcu_read_unlock();
}

void cfg80211_client_reg_change(struct wiphy *wiphy)
{
	struct wireless_dev *wdev;

	ASSERT_RTNL();

	list_for_each_entry(wdev, &wiphy->wdev_list, list) {
		struct cfg80211_client *client = rtnl_dereference(wdev->client);

		if (client && client->ops->reg_change)
			client->ops->reg_change(client->priv);
	}
}

void cfg80211_client_gone(struct wireless_dev *wdev)
{
	struct cfg80211_client *client;

	ASSERT_RTNL();

	client = wiphy_dereference(wdev->wiphy, wdev->client);
	if (!client)
		return;

	client->gone = true;
	RCU_INIT_POINTER(wdev->client, NULL);
	if (client->ops->gone)
		client->ops->gone(client->priv);
}
