#include <linux/etherdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/random.h>
#include <linux/timer.h>
#include <linux/version.h>
#include <linux/workqueue.h>
#include <net/cfg80211.h>

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("virtual cfg80211 driver");

#define NAME_PREFIX "owl"
#define NDEV_NAME NAME_PREFIX "%d"

#define DOT11_MGMT_HDR_LEN 24      /* d11 management header len */
#define DOT11_BCN_PRB_FIXED_LEN 12 /* beacon/probe fixed length */

#define MAX_PROBED_SSIDS 69
#define IE_MAX_LEN 512

#define SCAN_TIMEOUT_MS 100 /*< millisecond */

/* Note: owl_cipher_suites is an array of int defining which cipher suites
 * are supported. A pointer to this array and the number of entries is passed
 * on to upper layers.
 */
static const u32 owl_cipher_suites[] = {
    WLAN_CIPHER_SUITE_WEP40,
    WLAN_CIPHER_SUITE_WEP104,
    WLAN_CIPHER_SUITE_TKIP,
    WLAN_CIPHER_SUITE_CCMP,
};

struct owl_packet {
    int datalen;
    u8 data[ETH_DATA_LEN];
    struct list_head list;
};

enum vwifi_state { OWL_READY, OWL_SHUTDOWN };

/* Context for the whole program, so there's only single owl_context
 * regardless of the number of virtual interfaces. Fields in the structure
 * are interface-independent.
 */
struct owl_context {
    /* We may not need this lock because vif_list would not change during
     * the whole lifetime.
     */
    struct mutex lock;
    enum vwifi_state state;    /**< indicate the program state */
    struct list_head vif_list; /**< maintaining all interfaces */
    struct list_head ap_list;  /**< maintaining multiple AP */
};

/* SME stands for "station management entity" */
enum sme_state { SME_DISCONNECTED, SME_CONNECTING, SME_CONNECTED };

/* Virtual interface pointed to by netdev_priv(). Fields in the structure are
 * interface-dependent. Every interface has its own owl_vif, regardless of the
 * interface mode (STA, AP, Ad-hoc...).
 */
struct owl_vif {
    struct wireless_dev wdev;
    struct net_device *ndev;
    struct net_device_stats stats;

    size_t ssid_len;
    /* Currently connected BSS id */
    u8 bssid[ETH_ALEN];
    u8 ssid[IEEE80211_MAX_SSID_LEN];

    struct list_head rx_queue; /**< Head of received packet queue */
    /* Store all owl_vif which is in the same BSS (AP will be the head). */
    struct list_head bss_list;
    /* List entry for maintaining all owl_vif, which can be accessed via
     * owl->vif_list.
     */
    struct list_head list;

    struct mutex lock;

    /* Split logic for STA and AP mode */
    union {
        /* Structure for STA mode */
        struct {
            /* For the case the STA is going to roam to another BSS */
            u8 req_ssid[IEEE80211_MAX_SSID_LEN];

            struct cfg80211_scan_request *scan_request;
            enum sme_state sme_state; /* connection information */
            /* last connection time to a AP (in jiffies) */
            unsigned long conn_time;
            unsigned long active_time; /**< last tx/rx time (in jiffies) */
            u16 disconnect_reason_code;

            struct timer_list scan_timeout;
            struct work_struct ws_connect, ws_disconnect;
            struct work_struct ws_scan, ws_scan_timeout;

            /* For quickly finding the AP */
            struct owl_vif *ap;
        };
        /* Structure for AP mode */
        struct {
            bool ap_enabled;
            bool privacy;
            /* List node for storing AP (owl->ap_list is the head),
             * this field is for interface in AP mode.
             */
            struct list_head ap_list;
        };
    };

    u32 beacon_ie_len;
    u8 beacon_ie[IE_MAX_LEN];
};

static int station = 2;
module_param(station, int, 0444);
MODULE_PARM_DESC(station, "Number of virtual interfaces running in STA mode.");

/* Global context */
static struct owl_context *owl = NULL;

/* helper function to retrieve vif from net_device */
static inline struct owl_vif *ndev_get_owl_vif(struct net_device *ndev)
{
    return (struct owl_vif *) netdev_priv(ndev);
}

/* helper function to retrieve vif from wireless_dev */
static inline struct owl_vif *wdev_get_owl_vif(struct wireless_dev *wdev)
{
    return container_of(wdev, struct owl_vif, wdev);
}

#define SIN_S3_MIN (-(1 << 12))
#define SIN_S3_MAX (1 << 12)

/* A sine approximation via a third-order approx.
 * Refer to https://www.coranac.com/2009/07/sines for details about the
 * algorithm. Some parameters have been adjusted to increase the frequency
 * of the sine function.
 * Note: __sin_s3() is intended for internal use by rand_int_smooth() and
 * should not be called elsewhere.
 *
 * @x: seed to generate third-order sine value
 * @return: signed 32-bit integer ranging from SIN_S3_MIN to SIN_S3_MAX
 */
static inline s32 __sin_s3(s32 x)
{
    /* S(x) = (x * (3 * 2^p - (x * x)/2^r)) / 2^s
     * @n: the angle scale
     * @A: the amplitude
     * @p: keep the multiplication from overflowing
     */
    const int32_t n = 6, A = 12, p = 10, r = 2 * n - p, s = n + p + 1 - A;

    x = x << (30 - n);

    if ((x ^ (x << 1)) < 0)
        x = (1 << 31) - x;

    x = x >> (30 - n);
    return (x * ((3 << p) - ((x * x) >> r))) >> s;
}

/* Generate a signed 32-bit integer by feeding the seed into __sin_s3().
 * The distribution of (seed, rand_int_smooth()) is closer to a sine function
 * when plotted.
 */
static inline s32 rand_int_smooth(s32 low, s32 up, s32 seed)
{
    s32 result = __sin_s3(seed) - SIN_S3_MIN;
    result = (result * (up - low)) / (SIN_S3_MAX - SIN_S3_MIN);
    result += low;
    return result;
}

/* Helper function that prepares a structure with self-defined BSS information
 * and "informs" the kernel about the "new" BSS. Most of the code is copied from
 * the upcoming inform_dummy_bss function.
 */
static void inform_bss(struct owl_vif *vif)
{
    struct owl_vif *ap;

    list_for_each_entry (ap, &owl->ap_list, ap_list) {
        struct cfg80211_bss *bss = NULL;
        struct cfg80211_inform_bss data = {
            /* the only channel */
            .chan = &ap->wdev.wiphy->bands[NL80211_BAND_2GHZ]->channels[0],
            .scan_width = NL80211_BSS_CHAN_WIDTH_20,
            .signal = DBM_TO_MBM(rand_int_smooth(-100, -30, jiffies)),
        };
        int capability = WLAN_CAPABILITY_ESS;

        if (ap->privacy)
            capability |= WLAN_CAPABILITY_PRIVACY;

        pr_info("owl: %s performs scan, found %s (SSID: %s, BSSID: %pM)\n",
                vif->ndev->name, ap->ndev->name, ap->ssid, ap->bssid);
        pr_info("cap = %d, beacon_ie_len = %d\n", capability,
                ap->beacon_ie_len);

        /* Using the CLOCK_BOOTTIME clock, which remains unaffected by changes
         * in the system time-of-day clock and includes any time that the
         * system is suspended.
         * This clock is suitable for synchronizing the machines in the BSS
         * using tsf.
         */
        u64 tsf = div_u64(ktime_get_boottime_ns(), 1000);

        /* It is possible to use cfg80211_inform_bss() instead. */
        bss = cfg80211_inform_bss_data(
            vif->wdev.wiphy, &data, CFG80211_BSS_FTYPE_UNKNOWN, ap->bssid, tsf,
            capability, 100, ap->beacon_ie, ap->beacon_ie_len, GFP_KERNEL);

        /* cfg80211_inform_bss_data() returns cfg80211_bss structure reference
         * counter of which should be decremented if it is unused.
         */
        cfg80211_put_bss(vif->wdev.wiphy, bss);
    }
}

static int owl_ndo_open(struct net_device *dev)
{
    netif_start_queue(dev);
    return 0;
}

static int owl_ndo_stop(struct net_device *dev)
{
    struct owl_vif *vif = ndev_get_owl_vif(dev);
    struct owl_packet *pkt, *is = NULL;
    list_for_each_entry_safe (pkt, is, &vif->rx_queue, list) {
        list_del(&pkt->list);
        kfree(pkt);
    }
    netif_stop_queue(dev);
    return 0;
}

static struct net_device_stats *owl_ndo_get_stats(struct net_device *dev)
{
    struct owl_vif *vif = ndev_get_owl_vif(dev);
    return &vif->stats;
}

static netdev_tx_t owl_ndo_start_xmit(struct sk_buff *skb,
                                      struct net_device *dev);

/* Receive a packet: retrieve, encapsulate it in an skb, and perform the
 * following operations based on the interface mode:
 *   - STA mode: Pass the skb to the upper level (protocol stack).
 *   - AP mode: Perform the following operations based on the packet type:
 *     1. Unicast: If the skb is intended for another STA, pass it to that
 *        STA and do not pass it to the protocol stack. If the skb is intended
 *        for the AP itself, pass it to the protocol stack.
 *     2. Broadcast: Pass the skb to all other STAs except the source STA, and
 *        then pass it to the protocol stack.
 *     3. Multicast: Perform the same operations as for broadcast.
 */
static void owl_rx(struct net_device *dev)
{
    struct owl_vif *vif = ndev_get_owl_vif(dev);
    /* socket buffer will be sended to protocol stack */
    struct sk_buff *skb;
    /* socket buffer will be transmitted to another STA */
    struct sk_buff *skb1 = NULL;
    struct owl_packet *pkt;

    if (list_empty(&vif->rx_queue)) {
        pr_info("owl rx: No packet in rx_queue\n");
        return;
    }

    if (mutex_lock_interruptible(&vif->lock))
        goto pkt_free;

    pkt = list_first_entry(&vif->rx_queue, struct owl_packet, list);

    vif->stats.rx_packets++;
    vif->stats.rx_bytes += pkt->datalen;
    vif->active_time = jiffies;

    mutex_unlock(&vif->lock);

    /* Put raw packet into socket buffer */
    skb = dev_alloc_skb(pkt->datalen + 2);
    if (!skb) {
        pr_info("owl rx: low on mem - packet dropped\n");
        vif->stats.rx_dropped++;
        goto pkt_free;
    }
    skb_reserve(skb, 2); /* align IP on 16B boundary */
    memcpy(skb_put(skb, pkt->datalen), pkt->data, pkt->datalen);

    list_del(&pkt->list);
    kfree(pkt);

    if (vif->wdev.iftype == NL80211_IFTYPE_AP) {
        struct ethhdr *eth_hdr = (struct ethhdr *) skb->data;

        /* When receiving a multicast/broadcast packet, it is sent to every
         * STA except the source STA, and then passed to the protocol stack.
         */
        if (is_multicast_ether_addr(eth_hdr->h_dest)) {
            pr_info("owl: is_multicast_ether_addr\n");
            skb1 = skb_copy(skb, GFP_KERNEL);
        }
        /* Receiving a unicast packet */
        else {
            /* The packet is not intended for the AP itself. Instead, it is
             * sent to the destination STA and not passed to the protocol stack.
             */
            if (!ether_addr_equal(eth_hdr->h_dest, vif->ndev->dev_addr)) {
                skb1 = skb;
                skb = NULL;
            }
        }

        if (skb1) {
            pr_info("owl: AP %s relay:\n", vif->ndev->name);
            owl_ndo_start_xmit(skb1, vif->ndev);
        }

        /* Nothing to pass to protocol stack */
        if (!skb)
            return;
    }

    /* Pass the skb to protocol stack */
    skb->dev = dev;
    skb->protocol = eth_type_trans(skb, dev);
    skb->ip_summed = CHECKSUM_UNNECESSARY; /* don't check it */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 18, 0)
    netif_rx_ni(skb);
#else
    netif_rx(skb);
#endif

    return;

pkt_free:
    list_del(&pkt->list);
    kfree(pkt);
}

static int __owl_ndo_start_xmit(struct owl_vif *vif,
                                struct owl_vif *dest_vif,
                                struct sk_buff *skb)
{
    struct owl_packet *pkt = NULL;
    struct ethhdr *eth_hdr = (struct ethhdr *) skb->data;
    int datalen;

    if (vif->wdev.iftype == NL80211_IFTYPE_STATION) {
        pr_info("owl: STA %s (%pM) send packet to AP %s (%pM)\n",
                vif->ndev->name, eth_hdr->h_source, dest_vif->ndev->name,
                eth_hdr->h_dest);
    } else if (vif->wdev.iftype == NL80211_IFTYPE_AP) {
        pr_info("owl: AP %s (%pM) send packet to STA %s (%pM)\n",
                vif->ndev->name, eth_hdr->h_source, dest_vif->ndev->name,
                eth_hdr->h_dest);
    }

    pkt = kmalloc(sizeof(struct owl_packet), GFP_KERNEL);
    if (!pkt) {
        pr_info("Ran out of memory allocating packet pool\n");
        return NETDEV_TX_OK;
    }
    datalen = skb->len;
    memcpy(pkt->data, skb->data, datalen);
    pkt->datalen = datalen;

    /* enqueue packet to destination vif's rx_queue */
    if (mutex_lock_interruptible(&dest_vif->lock))
        goto error_before_rx_queue;

    list_add_tail(&pkt->list, &dest_vif->rx_queue);

    mutex_unlock(&dest_vif->lock);

    if (mutex_lock_interruptible(&vif->lock))
        goto erorr_after_rx_queue;

    /* Update interface statistics */
    vif->stats.tx_packets++;
    vif->stats.tx_bytes += datalen;
    vif->active_time = jiffies;

    mutex_unlock(&vif->lock);

    if (dest_vif->wdev.iftype == NL80211_IFTYPE_STATION) {
        pr_info("owl: STA %s (%pM) receive packet from AP %s (%pM)\n",
                dest_vif->ndev->name, eth_hdr->h_dest, vif->ndev->name,
                eth_hdr->h_source);
    } else if (dest_vif->wdev.iftype == NL80211_IFTYPE_AP) {
        pr_info("owl: AP %s (%pM) receive packet from STA %s (%pM)\n",
                dest_vif->ndev->name, eth_hdr->h_dest, vif->ndev->name,
                eth_hdr->h_source);
    }

    /* Directly send to rx_queue, simulate the rx interrupt */
    owl_rx(dest_vif->ndev);

    return datalen;

erorr_after_rx_queue:
    list_del(&pkt->list);
error_before_rx_queue:
    kfree(pkt);
    return 0;
}

/* Network packet transmit.
 * Callback called by the kernel when packets need to be sent.
 */
static netdev_tx_t owl_ndo_start_xmit(struct sk_buff *skb,
                                      struct net_device *dev)
{
    struct owl_vif *vif = ndev_get_owl_vif(dev);
    struct owl_vif *dest_vif = NULL;
    struct ethhdr *eth_hdr = (struct ethhdr *) skb->data;
    int count = 0;

    /* TX by interface of STA mode */
    if (vif->wdev.iftype == NL80211_IFTYPE_STATION) {
        if (vif->ap && vif->ap->ap_enabled) {
            dest_vif = vif->ap;

            if (__owl_ndo_start_xmit(vif, dest_vif, skb))
                count++;
        }
    }
    /* TX by interface of AP mode */
    else if (vif->wdev.iftype == NL80211_IFTYPE_AP) {
        /* Check if the packet is broadcasting */
        if (is_broadcast_ether_addr(eth_hdr->h_dest)) {
            list_for_each_entry (dest_vif, &vif->bss_list, bss_list) {
                /* Don't send broadcast packet back to the source interface.
                 */
                if (ether_addr_equal(eth_hdr->h_source,
                                     dest_vif->ndev->dev_addr))
                    continue;

                if (__owl_ndo_start_xmit(vif, dest_vif, skb))
                    count++;
            }
        }
        /* The packet is unicasting */
        else {
            list_for_each_entry (dest_vif, &vif->bss_list, bss_list) {
                if (ether_addr_equal(eth_hdr->h_dest,
                                     dest_vif->ndev->dev_addr)) {
                    if (__owl_ndo_start_xmit(vif, dest_vif, skb))
                        count++;
                    break;
                }
            }
        }
    }

    if (!count)
        vif->stats.tx_dropped++;

    /* Don't forget to cleanup skb, as its ownership moved to xmit callback. */
    dev_kfree_skb(skb);

    return NETDEV_TX_OK;
}

/* Structure of functions for network devices.
 * It should have at least ndo_start_xmit functions called for packet to be
 * sent.
 */
static struct net_device_ops owl_ndev_ops = {
    .ndo_open = owl_ndo_open,
    .ndo_stop = owl_ndo_stop,
    .ndo_start_xmit = owl_ndo_start_xmit,
    .ndo_get_stats = owl_ndo_get_stats,
};

/* Inform the "dummy" BSS to kernel and call cfg80211_scan_done() to finish
 * scan.
 */
static void owl_scan_timeout_work(struct work_struct *w)
{
    struct owl_vif *vif = container_of(w, struct owl_vif, ws_scan_timeout);
    struct cfg80211_scan_info info = {
        /* if scan was aborted by user (calling cfg80211_ops->abort_scan) or by
         * any driver/hardware issue - field should be set to "true"
         */
        .aborted = false,
    };

    /* inform with dummy BSS */
    inform_bss(vif);

    if (mutex_lock_interruptible(&vif->lock))
        return;

    /* finish scan */
    cfg80211_scan_done(vif->scan_request, &info);

    vif->scan_request = NULL;

    mutex_unlock(&vif->lock);
}

/* Callback called when the scan timer timeouts. This function just schedules
 * the timeout work and offloads the job of informing "dummy" BSS to kernel
 * onto it.
 */
static void owl_scan_timeout(struct timer_list *t)
{
    struct owl_vif *vif = container_of(t, struct owl_vif, scan_timeout);

    if (vif->scan_request)
        schedule_work(&vif->ws_scan_timeout);
}

/* Scan routine. It simulates a fake BSS scan (in fact, it does nothing) and
 * sets a scan timer to start from then. Once the timer timeouts, the timeout
 * routine owl_scan_timeout() will be invoked. This routine schedules a timeout
 * work that informs the kernel about the "dummy" BSS and completes the scan.
 */
static void owl_scan_routine(struct work_struct *w)
{
    struct owl_vif *vif = container_of(w, struct owl_vif, ws_scan);

    /* In a real-world driver, BSS scanning would occur here. However, in the
     * case of viwifi, scanning is not performed because dummy BSS entries are
     * already stored in the SSID hash table. Instead, a scan timeout is set
     * after a specific number of jiffies. The timeout worker informs the
     * kernel about the "dummy" BSS and calls cfg80211_scan_done() to complete
     * the scan.
     */
    mod_timer(&vif->scan_timeout, jiffies + msecs_to_jiffies(SCAN_TIMEOUT_MS));
}

static void owl_connect_routine(struct work_struct *w)
{
    struct owl_vif *vif = container_of(w, struct owl_vif, ws_connect);
    struct owl_vif *ap = NULL;
    struct station_info *sinfo;

    if (mutex_lock_interruptible(&vif->lock))
        return;

    /* Finding the AP by request SSID */
    list_for_each_entry (ap, &owl->ap_list, ap_list) {
        if (!memcmp(ap->ssid, vif->req_ssid, ap->ssid_len)) {
            pr_info("owl: %s is connected to AP %s (SSID: %s, BSSID: %pM)\n",
                    vif->ndev->name, ap->ndev->name, ap->ssid, ap->bssid);

            if (mutex_lock_interruptible(&ap->lock))
                return;

            /* AP connection part */
            sinfo = kmalloc(sizeof(struct station_info), GFP_KERNEL);
            if (!sinfo)
                return;

            /* It is safe that we fake the association request IEs
             * by beacon IEs, since they both possibly have the WPA/RSN IE
             * which is what the upper user-space program (e.g. hostapd)
             * cares about.
             */
            sinfo->assoc_req_ies = ap->beacon_ie;
            sinfo->assoc_req_ies_len = ap->beacon_ie_len;

            list_add_tail(&vif->bss_list, &ap->bss_list);

            /* nl80211 will inform the user-space program (e.g. hostapd)
             * about the newly-associated station via generic netlink
             * command NL80211_CMD_NEW_STATION for latter processing
             * (e.g. 4-way handshake).
             */
            cfg80211_new_sta(ap->ndev, vif->ndev->dev_addr, sinfo, GFP_KERNEL);

            mutex_unlock(&ap->lock);

            /* STA conneciton part */
            cfg80211_connect_result(vif->ndev, ap->bssid, NULL, 0, NULL, 0,
                                    WLAN_STATUS_SUCCESS, GFP_KERNEL);
            memcpy(vif->ssid, ap->ssid, ap->ssid_len);
            memcpy(vif->bssid, ap->bssid, ETH_ALEN);
            vif->sme_state = SME_CONNECTED;
            vif->conn_time = jiffies;
            vif->ap = ap;

            mutex_unlock(&vif->lock);

            kfree(sinfo);

            return;
        }
    }

    /* SSID not found */
    pr_info("owl: SSID %s not found\n", vif->req_ssid);

    cfg80211_connect_timeout(vif->ndev, NULL, NULL, 0, GFP_KERNEL,
                             NL80211_TIMEOUT_SCAN);
    vif->sme_state = SME_DISCONNECTED;
    mutex_unlock(&vif->lock);
}

/* Invoke cfg80211_disconnected() that informs the kernel that disconnect is
 * complete. Overall disconnect may call cfg80211_connect_timeout() if
 * disconnect interrupting connection routine, but for this module let's keep
 * it simple as possible. This routine is called through workqueue, when the
 * kernel asks to disconnect through cfg80211_ops.
 */
static void owl_disconnect_routine(struct work_struct *w)
{
    struct owl_vif *vif = container_of(w, struct owl_vif, ws_disconnect);

    pr_info("owl: %s disconnected from AP %s\n", vif->ndev->name,
            vif->ap->ndev->name);

    if (mutex_lock_interruptible(&vif->lock))
        return;

    /* STA cleanup stuff */
    cfg80211_disconnected(vif->ndev, vif->disconnect_reason_code, NULL, 0, true,
                          GFP_KERNEL);

    vif->disconnect_reason_code = 0;
    vif->sme_state = SME_DISCONNECTED;

    /* AP cleanup stuff */
    if (owl->state != OWL_SHUTDOWN) {
        if (mutex_lock_interruptible(&vif->ap->lock)) {
            mutex_unlock(&vif->lock);
            return;
        }

        if (vif->ap->ap_enabled && !list_empty(&vif->bss_list)) {
            cfg80211_del_sta(vif->ap->ndev, vif->ndev->dev_addr, GFP_KERNEL);
            list_del(&vif->bss_list);
        }

        mutex_unlock(&vif->ap->lock);

        vif->ap = NULL;
    }

    mutex_unlock(&vif->lock);
}

/* callback called by the kernel when user decided to scan.
 * This callback should initiate scan routine(through work_struct) and exit with
 * 0 if everything is ok.
 */
static int owl_scan(struct wiphy *wiphy, struct cfg80211_scan_request *request)
{
    struct owl_vif *vif = wdev_get_owl_vif(request->wdev);

    if (mutex_lock_interruptible(&vif->lock))
        return -ERESTARTSYS;

    if (vif->scan_request) {
        mutex_unlock(&vif->lock);
        return -EBUSY;
    }

    if (vif->wdev.iftype == NL80211_IFTYPE_AP) {
        mutex_unlock(&vif->lock);
        return -EPERM;
    }
    vif->scan_request = request;

    mutex_unlock(&vif->lock);

    if (!schedule_work(&vif->ws_scan))
        return -EBUSY;
    return 0;
}

/* callback called by the kernel when there is need to "connect" to some
 * network. It initializes connection routine through work_struct and exits
 * with 0 if everything is ok. connect routine should be finished with
 * cfg80211_connect_bss()/cfg80211_connect_result()/cfg80211_connect_done() or
 * cfg80211_connect_timeout().
 */
static int owl_connect(struct wiphy *wiphy,
                       struct net_device *dev,
                       struct cfg80211_connect_params *sme)
{
    struct owl_vif *vif = ndev_get_owl_vif(dev);

    if (mutex_lock_interruptible(&vif->lock))
        return -ERESTARTSYS;

    if (vif->sme_state != SME_DISCONNECTED) {
        mutex_unlock(&vif->lock);
        return -EBUSY;
    }

    if (vif->wdev.iftype == NL80211_IFTYPE_AP) {
        mutex_unlock(&vif->lock);
        return -EPERM;
    }

    vif->sme_state = SME_CONNECTING;
    vif->ssid_len = sme->ssid_len;
    memcpy(vif->req_ssid, sme->ssid, sme->ssid_len);
    mutex_unlock(&vif->lock);

    if (!schedule_work(&vif->ws_connect))
        return -EBUSY;
    return 0;
}

/* callback called by the kernel when there is need to "diconnect" from
 * currently connected network. It initializes disconnect routine through
 * work_struct and exits with 0 if everything ok. disconnect routine should
 * call cfg80211_disconnected() to inform the kernel that disconnection is
 * complete.
 */
static int owl_disconnect(struct wiphy *wiphy,
                          struct net_device *dev,
                          u16 reason_code)
{
    struct owl_vif *vif = ndev_get_owl_vif(dev);

    if (mutex_lock_interruptible(&vif->lock))
        return -ERESTARTSYS;

    if (vif->sme_state == SME_DISCONNECTED) {
        mutex_unlock(&vif->lock);
        return -EINVAL;
    }

    if (vif->wdev.iftype == NL80211_IFTYPE_AP) {
        mutex_unlock(&vif->lock);
        return -EPERM;
    }

    vif->disconnect_reason_code = reason_code;

    mutex_unlock(&vif->lock);

    if (!schedule_work(&vif->ws_disconnect))
        return -EBUSY;

    return 0;
}

/* Callback called by the kernel when the user requests information about
 * a specific station. The information includes the number and bytes of
 * transmitted and received packets, signal strength, and timing information
 * such as inactive time and elapsed time since the last connection to an AP.
 * This callback is invoked when the rtnl lock has been acquired.
 */
static int owl_get_station(struct wiphy *wiphy,
                           struct net_device *dev,
                           const u8 *mac,
                           struct station_info *sinfo)
{
    struct owl_vif *vif = ndev_get_owl_vif(dev);

    bool found_sta = false;
    switch (dev->ieee80211_ptr->iftype) {
    case NL80211_IFTYPE_AP:;
        struct owl_vif *sta_vif = NULL;
        list_for_each_entry (sta_vif, &vif->bss_list, bss_list) {
            if (!memcmp(mac, sta_vif->ndev->dev_addr, ETH_ALEN)) {
                found_sta = true;
                break;
            }
        }
        if (!memcmp(mac, sta_vif->ndev->dev_addr, ETH_ALEN))
            found_sta = true;
        break;
    case NL80211_IFTYPE_STATION:
        if (!memcmp(mac, vif->bssid, ETH_ALEN))
            found_sta = true;
        break;
    default:
        pr_info("owl: invalid interface type %u\n", dev->ieee80211_ptr->iftype);
        return -EINVAL;
    }

    if (!found_sta)
        return -ENONET;

    sinfo->filled = BIT_ULL(NL80211_STA_INFO_TX_PACKETS) |
                    BIT_ULL(NL80211_STA_INFO_RX_PACKETS) |
                    BIT_ULL(NL80211_STA_INFO_TX_FAILED) |
                    BIT_ULL(NL80211_STA_INFO_TX_BYTES) |
                    BIT_ULL(NL80211_STA_INFO_RX_BYTES) |
                    BIT_ULL(NL80211_STA_INFO_SIGNAL) |
                    BIT_ULL(NL80211_STA_INFO_INACTIVE_TIME);

    if (vif->sme_state == SME_CONNECTED) {
        sinfo->filled |= BIT_ULL(NL80211_STA_INFO_CONNECTED_TIME);
        sinfo->connected_time =
            jiffies_to_msecs(jiffies - vif->conn_time) / 1000;
    }

    sinfo->tx_packets = vif->stats.tx_packets;
    sinfo->rx_packets = vif->stats.rx_packets;
    sinfo->tx_failed = vif->stats.tx_dropped;
    sinfo->tx_bytes = vif->stats.tx_bytes;
    sinfo->rx_bytes = vif->stats.rx_bytes;
    /* For CFG80211_SIGNAL_TYPE_MBM, value is expressed in dBm */
    sinfo->signal = rand_int_smooth(-100, -30, jiffies);
    sinfo->inactive_time = jiffies_to_msecs(jiffies - vif->active_time);
    /* TODO: Emulate rate and mcs */

    return 0;
}

/* dump station callback -- resume dump at index @idx */
static int owl_dump_station(struct wiphy *wiphy,
                            struct net_device *dev,
                            int idx,
                            u8 *mac,
                            struct station_info *sinfo)
{
    struct owl_vif *ap_vif = ndev_get_owl_vif(dev);

    pr_info("Dump station at the idx %d\n", idx);

    int ret = -ENONET;
    struct owl_vif *sta_vif = NULL;
    int i = 0;

    list_for_each_entry (sta_vif, &ap_vif->bss_list, bss_list) {
        if (i < idx) {
            ++i;
            continue;
        }
        break;
    }

    if (sta_vif == ap_vif)
        return ret;

    ret = 0;

    memcpy(mac, sta_vif->ndev->dev_addr, ETH_ALEN);
    return owl_get_station(wiphy, dev, mac, sinfo);
}

/* Create a virtual interface that has its own wiphy, not shared with other
 * interfaces. The interface mode is set to STA mode. To change the interface
 * type, use the change_virtual_intf() function.
 */
static struct wireless_dev *owinterface_add(struct wiphy *wiphy, int if_idx)
{
    struct net_device *ndev = NULL;
    struct owl_vif *vif = NULL;

    /* allocate network device context. */
    ndev = alloc_netdev(sizeof(struct owl_vif), NDEV_NAME, NET_NAME_ENUM,
                        ether_setup);

    if (!ndev)
        goto error_alloc_ndev;

    /* fill private data of network context. */
    vif = ndev_get_owl_vif(ndev);
    vif->ndev = ndev;

    /* fill wireless_dev context.
     * wireless_dev with net_device can be represented as inherited class of
     * single net_device.
     */
    vif->wdev.wiphy = wiphy;
    vif->wdev.netdev = ndev;
    vif->wdev.iftype = NL80211_IFTYPE_STATION;
    vif->ndev->ieee80211_ptr = &vif->wdev;

    /* set network device hooks. should implement ndo_start_xmit() at least */
    vif->ndev->netdev_ops = &owl_ndev_ops;

    /* Add here proper net_device initialization */
    vif->ndev->features |= NETIF_F_HW_CSUM;

    /* The first byte is '\0' to avoid being a multicast
     * address (the first byte of multicast addrs is odd).
     */
    char intf_name[ETH_ALEN] = {0};
    snprintf(intf_name + 1, ETH_ALEN, "%s%d", NAME_PREFIX, if_idx);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
    eth_hw_addr_set(vif->ndev, intf_name);
#else
    memcpy(vif->ndev->dev_addr, intf_name, ETH_ALEN);
#endif

    /* register network device. If everything is ok, there should be new
     * network device: $ ip a
     * owl0: <BROADCAST,MULTICAST> mtu 1500 qdisc
     *       noop state DOWN group default link/ether 00:00:00:00:00:00
     *       brd ff:ff:ff:ff:ff:ff
     */
    if (register_netdev(vif->ndev))
        goto error_ndev_register;

    /* Initialize connection information */
    memset(vif->bssid, 0, ETH_ALEN);
    memset(vif->ssid, 0, IEEE80211_MAX_SSID_LEN);
    memset(vif->req_ssid, 0, IEEE80211_MAX_SSID_LEN);
    vif->scan_request = NULL;
    vif->sme_state = SME_DISCONNECTED;
    vif->conn_time = 0;
    vif->active_time = 0;
    vif->disconnect_reason_code = 0;
    vif->ap = NULL;

    mutex_init(&vif->lock);

    /* Initialize timer of scan_timeout */
    timer_setup(&vif->scan_timeout, owl_scan_timeout, 0);

    INIT_WORK(&vif->ws_connect, owl_connect_routine);
    INIT_WORK(&vif->ws_disconnect, owl_disconnect_routine);
    INIT_WORK(&vif->ws_scan, owl_scan_routine);
    INIT_WORK(&vif->ws_scan_timeout, owl_scan_timeout_work);

    /* Initialize rx_queue */
    INIT_LIST_HEAD(&vif->rx_queue);

    /* Add vif into global vif_list */
    if (mutex_lock_interruptible(&owl->lock))
        goto error_add_list;
    list_add_tail(&vif->list, &owl->vif_list);
    mutex_unlock(&owl->lock);

    return &vif->wdev;

error_add_list:
    unregister_netdev(vif->ndev);
error_ndev_register:
    free_netdev(vif->ndev);
error_alloc_ndev:
    wiphy_unregister(wiphy);
    wiphy_free(wiphy);
    return NULL;
}

/* Called by kernel when user decided to change the interface type. */
static int owl_change_iface(struct wiphy *wiphy,
                            struct net_device *ndev,
                            enum nl80211_iftype type,
                            struct vif_params *params)
{
    switch (type) {
    case NL80211_IFTYPE_STATION:
    case NL80211_IFTYPE_AP:
        ndev->ieee80211_ptr->iftype = type;
        break;
    default:
        pr_info("owl: invalid interface type %u\n", type);
        return -EINVAL;
    }

    return 0;
}

/* Called by the kernel when the user wants to create an Access Point.
 * Currently, it adds an SSID to the SSID table to emulate the AP signal and
 * records the SSID in the owl_context.
 */
static int owl_start_ap(struct wiphy *wiphy,
                        struct net_device *ndev,
                        struct cfg80211_ap_settings *settings)
{
    struct owl_vif *vif = ndev_get_owl_vif(ndev);
    int ie_offset = DOT11_MGMT_HDR_LEN + DOT11_BCN_PRB_FIXED_LEN;
    int head_ie_len, tail_ie_len;

    pr_info("owl: %s start acting in AP mode.\n", ndev->name);
    pr_info("ctrlchn=%d, center=%d, bw=%d, beacon_interval=%d, dtim_period=%d,",
            settings->chandef.chan->hw_value, settings->chandef.center_freq1,
            settings->chandef.width, settings->beacon_interval,
            settings->dtim_period);
    pr_info("ssid=%s(%zu), auth_type=%d, inactivity_timeout=%d", settings->ssid,
            settings->ssid_len, settings->auth_type,
            settings->inactivity_timeout);

    if (settings->ssid == NULL)
        return -EINVAL;

    /* Seting up AP SSID and BSSID */
    vif->ssid_len = settings->ssid_len;
    memcpy(vif->ssid, settings->ssid, settings->ssid_len);
    memcpy(vif->bssid, vif->ndev->dev_addr, ETH_ALEN);

    /* AP is the head of vif->bss_list */
    INIT_LIST_HEAD(&vif->bss_list);

    /* Add AP to global ap_list */
    list_add_tail(&vif->ap_list, &owl->ap_list);

    vif->ap_enabled = true;

    vif->privacy = settings->privacy;

    /* cfg80211 and some upper user-space programs treat IEs as two-part:
     * 1. head: 802.11 beacon frame header + beacon IEs before TIM IE
     * 2. tail: beacon IEs after TIM IE
     * We combine them and store them in vif->beaon_ie.
     */
    head_ie_len = settings->beacon.head_len - ie_offset;
    tail_ie_len = settings->beacon.tail_len;

    if (likely(head_ie_len + tail_ie_len <= IE_MAX_LEN)) {
        vif->beacon_ie_len = head_ie_len + tail_ie_len;
        memset(vif->beacon_ie, 0, IE_MAX_LEN);
        memcpy(vif->beacon_ie, &settings->beacon.head[ie_offset], head_ie_len);
        memcpy(vif->beacon_ie + head_ie_len, settings->beacon.tail,
               tail_ie_len);

        pr_info(
            "%s: privacy = %x, head_ie_len (before TIM IE) = %d, tail_ie_len = "
            "%d",
            __func__, settings->privacy, head_ie_len, tail_ie_len);
    } else {
        pr_info("%s: IE exceed %d bytes!\n", __func__, IE_MAX_LEN);
        return 1;
    }

    return 0;
}

/* Called by the kernel when there is a need to "stop" from AP mode. It uses
 * the SSID to remove the AP node from the SSID table.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 2)
static int owl_stop_ap(struct wiphy *wiphy,
                       struct net_device *ndev,
                       unsigned int link_id)
#else
static int owl_stop_ap(struct wiphy *wiphy, struct net_device *ndev)
#endif
{
    struct owl_vif *vif = ndev_get_owl_vif(ndev);
    struct owl_vif *pos = NULL, *safe = NULL;

    pr_info("owl: %s stop acting in AP mode.\n", ndev->name);

    if (owl->state == OWL_READY) {
        /* Destroy bss_list first */
        list_for_each_entry_safe (pos, safe, &vif->bss_list, bss_list)
            list_del(&pos->bss_list);

        /* Remove ap from global ap_list */
        if (mutex_lock_interruptible(&owl->lock))
            return -ERESTARTSYS;

        list_del(&vif->ap_list);

        mutex_unlock(&owl->lock);
    }

    vif->ap_enabled = false;

    return 0;
}

static int owl_change_beacon(struct wiphy *wiphy,
                             struct net_device *ndev,
                             struct cfg80211_beacon_data *info)
{
    struct owl_vif *vif = ndev_get_owl_vif(ndev);
    int ie_offset = DOT11_MGMT_HDR_LEN + DOT11_BCN_PRB_FIXED_LEN;
    int head_ie_len, tail_ie_len;

    /* cfg80211 and some user-space programs treat IEs as two-part:
     * 1. head: 802.11 beacon frame header + beacon IEs before TIM IE
     * 2. tail: beacon IEs after TIM IE
     * We combine them and store them in vif->beaon_ie.
     */
    head_ie_len = info->head_len - ie_offset;
    tail_ie_len = info->tail_len;

    if (likely(head_ie_len + tail_ie_len <= IE_MAX_LEN)) {
        vif->beacon_ie_len = head_ie_len + tail_ie_len;
        memset(vif->beacon_ie, 0, IE_MAX_LEN);
        memcpy(vif->beacon_ie, &info->head[ie_offset], head_ie_len);
        memcpy(vif->beacon_ie + head_ie_len, info->tail, tail_ie_len);

        pr_info(
            "%s: head_ie_len (before TIM IE) = %d, tail_ie_len = "
            "%d",
            __func__, head_ie_len, tail_ie_len);
    } else {
        pr_info("%s: IE exceed %d bytes!\n", __func__, IE_MAX_LEN);
        return 1;
    }

    return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
static int owl_add_key(struct wiphy *wiphy,
                       struct net_device *ndev,
                       int link_id,
                       u8 key_idx,
                       bool pairwise,
                       const u8 *mac_addr,
                       struct key_params *params)
#else
static int owl_add_key(struct wiphy *wiphy,
                       struct net_device *ndev,
                       u8 key_idx,
                       bool pairwise,
                       const u8 *mac_addr,
                       struct key_params *params)
#endif
{
    return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
static int owl_del_key(struct wiphy *wiphy,
                       struct net_device *ndev,
                       int link_id,
                       u8 key_idx,
                       bool pairwise,
                       const u8 *mac_addr)
#else
static int owl_del_key(struct wiphy *wiphy,
                       struct net_device *ndev,
                       u8 key_idx,
                       bool pairwise,
                       const u8 *mac_addr)
#endif
{
    return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
static int owl_set_default_key(struct wiphy *wiphy,
                               struct net_device *ndev,
                               int link_id,
                               u8 key_idx,
                               bool unicast,
                               bool multicast)
#else
static int owl_set_default_key(struct wiphy *wiphy,
                               struct net_device *ndev,
                               u8 key_idx,
                               bool unicast,
                               bool multicast)
#endif
{
    return 0;
}

/* Unregister and free a virtual interface identified by @vif->ndev. */
static int owl_delete_interface(struct owl_vif *vif)
{
    struct owl_packet *pkt = NULL, *safe = NULL;
    struct wiphy *wiphy = vif->wdev.wiphy;


    if (vif->wdev.iftype == NL80211_IFTYPE_AP) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 2)
        owl_stop_ap(wiphy, vif->ndev, 0);
#else
        owl_stop_ap(wiphy, vif->ndev);
#endif
    } else if (vif->wdev.iftype == NL80211_IFTYPE_STATION) {
        if (mutex_lock_interruptible(&vif->lock))
            return -ERESTARTSYS;

        /* Stop TX queue, and delete the pending packets */
        netif_stop_queue(vif->ndev);
        list_for_each_entry_safe (pkt, safe, &vif->rx_queue, list) {
            list_del(&pkt->list);
            kfree(pkt);
        }
        /* If there's a pending scan, call cfg80211_scan_done to finish it. */
        if (vif->scan_request) {
            struct cfg80211_scan_info info = {.aborted = true};

            cfg80211_scan_done(vif->scan_request, &info);
            vif->scan_request = NULL;
        }

        /* Make sure that no work is queued */
        del_timer_sync(&vif->scan_timeout);
        cancel_work_sync(&vif->ws_connect);
        cancel_work_sync(&vif->ws_disconnect);
        cancel_work_sync(&vif->ws_scan);
        cancel_work_sync(&vif->ws_scan_timeout);

        mutex_unlock(&vif->lock);
    }

    /* Deallocate net_device */
    unregister_netdev(vif->ndev);
    free_netdev(vif->ndev);

    /* Deallocate wiphy device */
    wiphy_unregister(wiphy);
    wiphy_free(wiphy);

    return 0;
}

/* Structure of functions for FullMAC 80211 drivers. Functions implemented
 * along with fields/flags in the wiphy structure represent driver features.
 * This module can only perform "scan" and "connect". Some functions cannot
 * be implemented alone. For example, with "connect" there should be a
 * corresponding "disconnect" function.
 */
static struct cfg80211_ops owl_cfg_ops = {
    .change_virtual_intf = owl_change_iface,
    .scan = owl_scan,
    .connect = owl_connect,
    .disconnect = owl_disconnect,
    .get_station = owl_get_station,
    .dump_station = owl_dump_station,
    .start_ap = owl_start_ap,
    .stop_ap = owl_stop_ap,
    .change_beacon = owl_change_beacon,
    .add_key = owl_add_key,
    .del_key = owl_del_key,
    .set_default_key = owl_set_default_key,
};

/* Macro for defining channel array */
#define CHAN_2GHZ(_channel, _freq)                         \
    {                                                      \
        .band = NL80211_BAND_2GHZ, .hw_value = (_channel), \
        .center_freq = (_freq),                            \
    }

/* Macro for defining rate table */
#define RATE_ENT(_rate, _hw_value)                   \
    {                                                \
        .bitrate = (_rate), .hw_value = (_hw_value), \
    }

/* Array of "supported" channels in 2GHz band. It is required for wiphy. */
static struct ieee80211_channel owl_supported_channels_2ghz[] = {
    CHAN_2GHZ(1, 2412),  CHAN_2GHZ(2, 2417),  CHAN_2GHZ(3, 2422),
    CHAN_2GHZ(4, 2427),  CHAN_2GHZ(5, 2432),  CHAN_2GHZ(6, 2437),
    CHAN_2GHZ(7, 2442),  CHAN_2GHZ(8, 2447),  CHAN_2GHZ(9, 2452),
    CHAN_2GHZ(10, 2457), CHAN_2GHZ(11, 2462), CHAN_2GHZ(12, 2467),
    CHAN_2GHZ(13, 2472), CHAN_2GHZ(14, 2484),
};

/* Array of supported rates, required to support at least those next rates
 * for 2GHz band.
 */
static struct ieee80211_rate owl_supported_rates_2ghz[] = {
    RATE_ENT(10, 0x1),    RATE_ENT(20, 0x2),    RATE_ENT(55, 0x4),
    RATE_ENT(110, 0x8),   RATE_ENT(60, 0x10),   RATE_ENT(90, 0x20),
    RATE_ENT(120, 0x40),  RATE_ENT(180, 0x80),  RATE_ENT(240, 0x100),
    RATE_ENT(360, 0x200), RATE_ENT(480, 0x400), RATE_ENT(540, 0x800),
};

/* Describes supported band of 2GHz. */
static struct ieee80211_supported_band nf_band_2ghz = {
    /* FIXME: add other band capabilities if nedded, such as 40 width */
    .ht_cap.cap = IEEE80211_HT_CAP_SGI_20,
    .ht_cap.ht_supported = false,

    .channels = owl_supported_channels_2ghz,
    .n_channels = ARRAY_SIZE(owl_supported_channels_2ghz),

    .bitrates = owl_supported_rates_2ghz,
    .n_bitrates = ARRAY_SIZE(owl_supported_rates_2ghz),
};

/* Unregister and free virtual interfaces and wiphy. */
static void owl_free(void)
{
    struct owl_vif *vif = NULL, *safe = NULL;

    list_for_each_entry_safe (vif, safe, &owl->vif_list, list)
        owl_delete_interface(vif);

    kfree(owl);
}

/* Allocate and register wiphy.
 * Virtual interfaces should be created by nl80211, which will call
 * cfg80211_ops->add_iface(). This program creates a wiphy for every
 * virtual interface, which means a virtual interface has a physical (virtual)
 * adapter associated with it.
 */
static struct wiphy *owcfg80211_add(void)
{
    struct wiphy *wiphy = NULL;

    /* allocate wiphy context. It is possible just to use wiphy_new().
     * wiphy should represent physical FullMAC wireless device. We need
     * to implement add_virtual_intf() from cfg80211_ops for adding
     * interface(s) on top of a wiphy.
     * NULL means use the default phy%d naming.
     */
    wiphy = wiphy_new_nm(&owl_cfg_ops, 0, NULL);
    if (!wiphy) {
        pr_info("couldn't allocate wiphy device\n");
        return NULL;
    }

    /* FIXME: set device object as wiphy "parent" */
    /* set_wiphy_dev(ret->wiphy, dev); */

    /* wiphy should determinate its type.
     * add other required types like  "BIT(NL80211_IFTYPE_STATION) |
     * BIT(NL80211_IFTYPE_AP)" etc.
     */
    wiphy->interface_modes =
        BIT(NL80211_IFTYPE_STATION) | BIT(NL80211_IFTYPE_AP);

    /* wiphy should have at least 1 band.
     * Also fill NL80211_BAND_5GHZ if required. In this module, only 1 band
     * with 1 "channel"
     */
    wiphy->bands[NL80211_BAND_2GHZ] = &nf_band_2ghz;

    /* scan - if the device supports "scan", we need to define max_scan_ssids
     * at least.
     */
    wiphy->max_scan_ssids = MAX_PROBED_SSIDS;

    /* Signal type
     * CFG80211_SIGNAL_TYPE_UNSPEC allows us specify signal strength from 0 to
     * 100. The reasonable value for CFG80211_SIGNAL_TYPE_MBM is -3000 to -10000
     * (mdBm).
     */
    wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;

    wiphy->flags |= WIPHY_FLAG_NETNS_OK;

    wiphy->cipher_suites = owl_cipher_suites;
    wiphy->n_cipher_suites = ARRAY_SIZE(owl_cipher_suites);

    /* register wiphy, if everything ok - there should be another wireless
     * device in system. use command: $ iw list
     */
    if (wiphy_register(wiphy) < 0) {
        pr_info("couldn't register wiphy device\n");
        goto error_wiphy_register;
    }

    return wiphy;

error_wiphy_register:
    wiphy_free(wiphy);
    return NULL;
}

static int __init vwifi_init(void)
{
    owl = kmalloc(sizeof(struct owl_context), GFP_KERNEL);
    if (!owl) {
        pr_info("couldn't allocate space for owl_context\n");
        return -ENOMEM;
    }

    mutex_init(&owl->lock);
    INIT_LIST_HEAD(&owl->vif_list);
    INIT_LIST_HEAD(&owl->ap_list);

    for (int i = 0; i < station; i++) {
        struct wiphy *wiphy = owcfg80211_add();
        if (!wiphy)
            goto cfg80211_add;
        if (!owinterface_add(wiphy, i))
            goto interface_add;
    }

    owl->state = OWL_READY;

    return 0;

interface_add:
    /* FIXME: check for resource deallocation */
cfg80211_add:
    owl_free();
    return -1;
}

static void __exit vwifi_exit(void)
{
    owl->state = OWL_SHUTDOWN;
    owl_free();
}

module_init(vwifi_init);
module_exit(vwifi_exit);
