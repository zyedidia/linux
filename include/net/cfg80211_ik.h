/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cfg80211 in-kernel consumer.
 *
 * nl80211 is the only way to drive a wiphy from outside cfg80211, and it
 * assumes a userspace peer. A UML driver domain has no userspace: the
 * component that must issue scans and connects is itself kernel code,
 * exporting the results over VDUSE instead of over netlink.
 *
 * This is the seam for that: a small consumer API that submits requests
 * through the same rdev ops nl80211 uses, and reports results through
 * callbacks fed by the notification entry points drivers already call.
 * Only one consumer may be attached at a time.
 *
 * Requests run under the wiphy mutex and may sleep. Callbacks are
 * invoked in whatever context the driver reported from, which may be
 * atomic, so they must not block.
 */

#ifndef __NET_CFG80211_IK_H
#define __NET_CFG80211_IK_H

#include <net/cfg80211.h>

struct cfg80211_ik;

struct cfg80211_ik_ops {
	void (*scan_done)(void *priv, bool aborted);
	void (*connect_result)(void *priv, const u8 *bssid, u16 status,
			       const u8 *req_ie, size_t req_ie_len,
			       const u8 *resp_ie, size_t resp_ie_len);
	void (*disconnected)(void *priv, u16 reason, bool locally_generated,
			     const u8 *ie, size_t ie_len);
	/*
	 * The interface is being unregistered. The consumer must stop
	 * issuing requests; every entry point returns -ENODEV from here
	 * on. Called with the RTNL held, so it must not block on it.
	 */
	void (*iface_gone)(void *priv);
};

/*
 * Attach to a station-mode interface. @ifname selects one by name, or is
 * NULL to take the first that appears. Returns ERR_PTR on failure; in
 * particular -EPROBE_DEFER if no suitable interface exists yet.
 */
struct cfg80211_ik *cfg80211_ik_attach(const char *ifname,
				       const struct cfg80211_ik_ops *ops,
				       void *priv);
void cfg80211_ik_detach(struct cfg80211_ik *ik);

struct wiphy *cfg80211_ik_wiphy(struct cfg80211_ik *ik);
struct net_device *cfg80211_ik_netdev(struct cfg80211_ik *ik);

/*
 * Start a scan. @freqs lists centre frequencies in MHz; an empty list
 * means every enabled channel. The result arrives as ->scan_done(),
 * after which the entries can be read with cfg80211_ik_bss_iter().
 */
int cfg80211_ik_scan(struct cfg80211_ik *ik,
		     const struct cfg80211_ssid *ssids, int n_ssids,
		     const u32 *freqs, int n_freqs,
		     const u8 *ie, size_t ie_len);
int cfg80211_ik_abort_scan(struct cfg80211_ik *ik);

void cfg80211_ik_bss_iter(struct cfg80211_ik *ik,
			  void (*iter)(struct wiphy *wiphy,
				       struct cfg80211_bss *bss, void *data),
			  void *data);

/* The outcome arrives as ->connect_result(). */
int cfg80211_ik_connect(struct cfg80211_ik *ik,
			struct cfg80211_connect_params *sme);
int cfg80211_ik_disconnect(struct cfg80211_ik *ik, u16 reason);

int cfg80211_ik_add_key(struct cfg80211_ik *ik, u8 key_index, bool pairwise,
			const u8 *mac_addr, struct key_params *params);
int cfg80211_ik_del_key(struct cfg80211_ik *ik, u8 key_index, bool pairwise,
			const u8 *mac_addr);
int cfg80211_ik_set_default_key(struct cfg80211_ik *ik, u8 key_index,
				bool unicast, bool multicast);
int cfg80211_ik_set_default_mgmt_key(struct cfg80211_ik *ik, u8 key_index);

int cfg80211_ik_get_station(struct cfg80211_ik *ik, const u8 *mac,
			    struct station_info *sinfo);

/* Open or close the 802.1X controlled port for @mac. */
int cfg80211_ik_set_authorized(struct cfg80211_ik *ik, const u8 *mac,
			       bool authorized);

#endif /* __NET_CFG80211_IK_H */
