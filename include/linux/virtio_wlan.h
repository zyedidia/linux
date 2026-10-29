/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_VIRTIO_WLAN_H
#define _LINUX_VIRTIO_WLAN_H

#include <linux/errno.h>
#include <linux/ieee80211.h>
#include <linux/string.h>
#include <uapi/linux/virtio_wlan.h>

static inline int virtio_wlan_status_to_errno(u16 status)
{
	switch (status) {
	case VIRTIO_WLAN_S_OK:
		return 0;
	case VIRTIO_WLAN_S_INVALID:
		return -EINVAL;
	case VIRTIO_WLAN_S_UNSUPPORTED:
		return -EOPNOTSUPP;
	case VIRTIO_WLAN_S_BUSY:
		return -EBUSY;
	case VIRTIO_WLAN_S_ALREADY:
		return -EALREADY;
	case VIRTIO_WLAN_S_NETDOWN:
		return -ENETDOWN;
	case VIRTIO_WLAN_S_NOT_FOUND:
		return -ENOENT;
	case VIRTIO_WLAN_S_NOT_CONNECTED:
		return -ENOTCONN;
	case VIRTIO_WLAN_S_NO_MEMORY:
		return -ENOMEM;
	case VIRTIO_WLAN_S_NO_DEVICE:
		return -ENODEV;
	default:
		return -EIO;
	}
}

static inline u16 virtio_wlan_errno_to_status(int err)
{
	switch (err) {
	case 0:
		return VIRTIO_WLAN_S_OK;
	case -EINVAL:
	case -ERANGE:
	case -E2BIG:
		return VIRTIO_WLAN_S_INVALID;
	case -EOPNOTSUPP:
		return VIRTIO_WLAN_S_UNSUPPORTED;
	case -EBUSY:
		return VIRTIO_WLAN_S_BUSY;
	case -EALREADY:
		return VIRTIO_WLAN_S_ALREADY;
	case -ENETDOWN:
		return VIRTIO_WLAN_S_NETDOWN;
	case -ENOENT:
		return VIRTIO_WLAN_S_NOT_FOUND;
	case -ENOTCONN:
	case -ENOLINK:
		return VIRTIO_WLAN_S_NOT_CONNECTED;
	case -ENOMEM:
		return VIRTIO_WLAN_S_NO_MEMORY;
	case -ENODEV:
		return VIRTIO_WLAN_S_NO_DEVICE;
	default:
		return VIRTIO_WLAN_S_FAILED;
	}
}

/*
 * The host and the guest pass the IEs of BSS, CONNECT_RESULT and DISCONNECTED
 * events through the same allowlist. Each kept element must be at least as
 * long as its definition requires.
 */

/*
 * Returns the first HE Capabilities element of ies if it is valid, and NULL
 * otherwise.
 */
static inline const struct element *virtio_wlan_he_cap_elem(const u8 *ies,
							    size_t len)
{
	const struct element *elem;

	for_each_element_extid(elem, WLAN_EID_EXT_HE_CAPABILITY, ies, len)
		return ieee80211_he_capa_size_ok(elem->data + 1,
						 elem->datalen - 1) ? elem : NULL;
	return NULL;
}

static inline bool virtio_wlan_vendor_elem_ok(const u8 *data, u8 len)
{
	if (len < 4 || (data[0] << 16 | data[1] << 8 | data[2]) !=
		       WLAN_OUI_MICROSOFT)
		return false;

	switch (data[3]) {
	case WLAN_OUI_TYPE_MICROSOFT_WPA:
		return len >= 6;
	case WLAN_OUI_TYPE_MICROSOFT_WMM:
		/* These are the WMM Information and Parameter elements. */
		return (len >= 7 && data[4] == 0) || (len >= 24 && data[4] == 1);
	case WLAN_OUI_TYPE_MICROSOFT_WPS:
		return true;
	default:
		return false;
	}
}

/*
 * he is the result of virtio_wlan_he_cap_elem() for the blob. Only that HE
 * Capabilities element is kept, and an EHT Capabilities element is kept only
 * if it is valid for that one.
 */
static inline bool virtio_wlan_ext_elem_ok(const struct element *elem,
					   const struct element *he,
					   bool from_ap)
{
	const u8 *data = elem->data + 1;
	u8 len = elem->datalen - 1;

	switch (elem->data[0]) {
	case WLAN_EID_EXT_HE_CAPABILITY:
		return elem == he;
	case WLAN_EID_EXT_HE_OPERATION:
		return len >= sizeof(struct ieee80211_he_operation) &&
		       len >= ieee80211_he_oper_size(data) - 1;
	case WLAN_EID_EXT_EHT_CAPABILITY:
		return he && ieee80211_eht_capa_size_ok(he->data + 1, data, len,
							from_ap);
	case WLAN_EID_EXT_EHT_OPERATION:
		return ieee80211_eht_oper_size_ok(data, len);
	default:
		return false;
	}
}

static inline bool virtio_wlan_elem_ok(const struct element *elem,
				       const struct element *he, bool from_ap)
{
	u8 len = elem->datalen;

	switch (elem->id) {
	case WLAN_EID_SSID:
		return len <= IEEE80211_MAX_SSID_LEN;
	case WLAN_EID_SUPP_RATES:
		return len >= 1 && len <= 8;
	case WLAN_EID_EXT_SUPP_RATES:
	case WLAN_EID_DS_PARAMS:
	case WLAN_EID_EXT_CAPABILITY:
	case WLAN_EID_RSNX:
		return len >= 1;
	case WLAN_EID_COUNTRY:
		return len >= IEEE80211_COUNTRY_IE_MIN_LEN;
	case WLAN_EID_RSN:
		return len >= 2;
	case WLAN_EID_MOBILITY_DOMAIN:
		return len >= 3;
	case WLAN_EID_HT_CAPABILITY:
		return len >= sizeof(struct ieee80211_ht_cap);
	case WLAN_EID_HT_OPERATION:
		return len >= sizeof(struct ieee80211_ht_operation);
	case WLAN_EID_VHT_CAPABILITY:
		return len >= sizeof(struct ieee80211_vht_cap);
	case WLAN_EID_VHT_OPERATION:
		return len >= sizeof(struct ieee80211_vht_operation);
	case WLAN_EID_VENDOR_SPECIFIC:
		return virtio_wlan_vendor_elem_ok(elem->data, len);
	case WLAN_EID_EXTENSION:
		return len >= 1 && virtio_wlan_ext_elem_ok(elem, he, from_ap);
	default:
		return false;
	}
}

/*
 * Copies the allowed elements of ies to dst in order and returns the number
 * of bytes written. Copying stops at the first allowed element that does not
 * fit in size bytes, and then *truncated is set if truncated is not NULL.
 * from_ap is set for IEs that an AP sent. dst must not overlap ies.
 */
static inline size_t virtio_wlan_filter_elems(u8 *dst, size_t size,
					      const u8 *ies, size_t len,
					      bool from_ap, bool *truncated)
{
	const struct element *he = virtio_wlan_he_cap_elem(ies, len);
	const struct element *elem;
	size_t pos = 0;

	if (truncated)
		*truncated = false;

	for_each_element(elem, ies, len) {
		size_t n = sizeof(*elem) + elem->datalen;

		if (!virtio_wlan_elem_ok(elem, he, from_ap))
			continue;
		if (n > size - pos) {
			if (truncated)
				*truncated = true;
			break;
		}
		memcpy(dst + pos, elem, n);
		pos += n;
	}
	return pos;
}

#endif
