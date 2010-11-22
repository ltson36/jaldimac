/* 
 * jaldi 
 */
 
#include <linux/init.h>
#include <linux/moduleparam.h>
#include <linux/module.h>

#include <linux/kernel.h>
#include <linux/types.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>

#include "jaldi.h"

MODULE_AUTHOR("Shaddi Hasan");
MODULE_LICENSE("Dual BSD/GPL");

bool jaldi_setpower(struct jaldi_softc *sc, enum jaldi_power_mode mode)
{
	unsigned long flags;
	bool result;

	spin_lock_irqsave(&sc->sc_pm_lock, flags);
	result = jaldi_hw_setpower(sc->hw, mode);
	spin_unlock_irqrestore(&sc->sc_pm_lock, flags);
	return result;
}

void jaldi_ps_wakeup(struct jaldi_softc *sc)
{
	unsigned long flags;
	spin_lock_irqsave(&sc->sc_pm_lock, flags);
	if (++sc->ps_usecount != 1)
		goto unlock;

	jaldi_hw_setpower(sc->hw, JALDI_PM_AWAKE);

 unlock:
	spin_unlock_irqrestore(&sc->sc_pm_lock, flags);
}

/* Maintains a priority queue of jaldi_packets to be sent */
void jaldi_tx_enqueue(struct jaldi_softc *sc, struct jaldi_packet *pkt) {
	unsigned long flags;
	
	spin_lock_irqsave(&sc->sc_netdevlock, flags);
	if(sc->tx_queue == NULL || pkt->tx_time < sc->tx_queue->tx_time){
		/* New packet goes in front */
		pkt->next = sc->tx_queue;
		sc->tx_queue = pkt;
	} else {
		struct jaldi_packet *curr = sc->tx_queue;
		while(curr->next != NULL && pkt->tx_time < curr->next->tx_time){	
			curr = curr->next;
		}
		pkt->next = curr->next;
		curr->next = pkt;
	}
	spin_unlock_irqrestore(&sc->sc_netdevlock, flags);
}

/* Returns the next jaldi_packet to be transmitted */
struct jaldi_packet *jaldi_tx_dequeue(struct jaldi_softc *sc) {
	struct jaldi_packet *pkt;
	unsigned long flags;
	
	spin_lock_irqsave(&sc->sc_netdevlock, flags);
	pkt = sc->tx_queue;
	if (pkt != NULL) sc->tx_queue = pkt->next;
	spin_unlock_irqrestore(&sc->sc_netdevlock, flags);
	return pkt;
}


irqreturn_t jaldi_isr(int irq, void *dev)
{
	struct jaldi_softc *sc = dev;
	struct jaldi_hw *hw = sc->hw;

	if (hw->dev_state != JALDI_HW_INITIALIZED) { return IRQ_NONE; }
	
	/* shared irq, not for us */
	if(!jaldi_hw_intrpend(hw)) { return IRQ_NONE; }

	// TODO: determine how we want to handle interrupts
}

	

int jaldi_release(struct net_device *dev)
{
	netif_stop_queue(dev);
	return 0;
}

/* Method stub from snull -- call from interrupt handler */
void jaldi_rx(struct net_device *dev, struct jaldi_packet *pkt)
{
	struct sk_buff *skb;
	struct jaldi_softc *sc = netdev_priv(dev);
	
	skb = dev_alloc_skb(pkt->datalen+2);
	
	if(!skb) {
		if (printk_ratelimit())
			printk(KERN_NOTICE "jaldi_rx: low on mem - packet dropped\n");
		sc->stats.rx_dropped++;
		goto out;
	}
	
	skb_reserve(skb,2);
	memcpy(skb_put(skb,pkt->datalen),pkt->data,pkt->datalen);
	
	skb->dev = dev;
	skb->protocol = eth_type_trans(skb, dev);
	skb->ip_summed = CHECKSUM_NONE;
	sc->stats.rx_packets++;
	sc->stats.rx_bytes += pkt->datalen;
	netif_rx(skb);
  out:
  	return;
}

/******/
/* TX */
/******/
struct jaldi_txq *jaldi_txq_setup(struct jaldi_softc *sc, int qtype, int subtype)
{
	struct jaldi_hw *hw = sc->hw;
	struct jaldi_tx_queue_info qi;
	int qnum, i;

	memset(&qi, 0, sizeof(qi));
	qi.tqi_subtype = subtype;
	qi.tqi_aifs = JALDI_TXQ_USEDEFAULT;
	qi.tqi_cwmin = JALDI_TXQ_USEDEFAULT;
	qi.tqi_cwmax = JALDI_TXQ_USEDEFAULT;
	qi.tqi_physCompBuf = 0;

	/* From ath9k:
	 * "Enable interrupts only for EOL and DESC conditions.
	 * We mark tx descriptors to receive a DESC interrupt
	 * when a tx queue gets deep; otherwise waiting for the
	 * EOL to reap descriptors.  Note that this is done to
	 * reduce interrupt load and this only defers reaping
	 * descriptors, never transmitting frames.  Aside from
	 * reducing interrupts this also permits more concurrency.
	 * The only potential downside is if the tx queue backs
	 * up in which case the top half of the kernel may backup
	 * due to a lack of tx descriptors.
	 *
	 * The UAPSD queue is an exception, since we take a desc-
	 * based intr on the EOSP frames." (note, we don't have uapsd)
	 */
	if (hw->caps.hw_caps & JALDI_HW_CAP_EDMA) {
		qi.tqi_qflags = TXQ_FLAG_TXOKINT_ENABLE |
				TXQ_FLAG_TXERRINT_ENABLE;
	} else {
		qi.tqi_qflags = TXQ_FLAG_TXEOLINT_ENABLE |
				TXQ_FLAG_TXDESCINT_ENABLE;
	}
	qnum = jaldi_hw_setuptxqueue(hw, qtype, &qi);
	if (qnum == -1) {
		/*
		 * NB: don't print a message, this happens
		 * normally on parts with too few tx queues
		 */
		jaldi_print(JALDI_DEBUG, "txq setup failed.\n"); /* just for debugging... */
		return NULL;
	}
	if (qnum >= ARRAY_SIZE(sc->tx.txq)) {
		jaldi_print(JALDI_FATAL,
			  "qnum %u out of range, max %u!\n",
			  qnum, (unsigned int)ARRAY_SIZE(sc->tx.txq));
		jaldi_hw_releasetxqueue(hw, qnum);
		return NULL;
	}
	if (!JALDI_TXQ_SETUP(sc, qnum)) {
		struct jaldi_txq *txq = &sc->tx.txq[qnum];

		txq->axq_qnum = qnum;
		txq->axq_link = NULL;
		INIT_LIST_HEAD(&txq->axq_q);
		INIT_LIST_HEAD(&txq->axq_acq);
		spin_lock_init(&txq->axq_lock);
		txq->axq_depth = 0;
		txq->axq_tx_inprogress = false;
		sc->tx.txqsetup |= 1<<qnum;

		txq->txq_headidx = txq->txq_tailidx = 0;
		for (i = 0; i < JALDI_TXFIFO_DEPTH; i++)
			INIT_LIST_HEAD(&txq->txq_fifo[i]);
		INIT_LIST_HEAD(&txq->txq_fifo_pending);
	}
	return &sc->tx.txq[qnum];
}

int jaldi_tx_setup(struct jaldi_softc *sc, int haltype)
{
	struct jaldi_txq *txq;

	DBG_START_MSG;

	if (haltype >= ARRAY_SIZE(sc->tx.hwq_map)) {
		jaldi_print(JALDI_FATAL,
			  "HAL AC %u out of range, max %zu!\n",
			 haltype, ARRAY_SIZE(sc->tx.hwq_map));
		return 0;
	}
	txq = jaldi_txq_setup(sc, JALDI_TX_QUEUE_DATA, haltype);
	if (txq != NULL) {
		sc->tx.hwq_map[haltype] = txq->axq_qnum;
		return 1;
	} else
		return 0;
}

void jaldi_tx_cleanupq(struct jaldi_softc *sc, struct jaldi_txq *txq)
{
	jaldi_hw_releasetxqueue(sc->hw, txq->axq_qnum);
	sc->tx.txqsetup &= ~(1<<txq->axq_qnum);
}

static int jaldi_tx_setup_buffer(struct jaldi_softc *sc, struct jaldi_buf *bf,
					struct jaldi_pkt *pkt)
{
	struct jaldi_hw *hw = sc->hw;

	JALDI_TXBUF_RESET(bf);

	bf->

}

static struct jaldi_buf *jaldi_tx_get_buffer(struct jaldi_softc *sc)
{
	struct jaldi_buf *bf = NULL;

	spin_lock_bh(&sc->tx.txbuflock);

	if (unlikely(list_empty(&sc->tx.txbuf))) {
		spin_unlock_bh(&sc->tx.txbuflock);
		jaldi_print(JALDI_DEBUG, "txbuf list empty\n");
		return NULL;
	}

	bf = list_first_entry(&sc->tx.txbuf, struct jaldi_buf, list);
	list_del(&bf->list);

	spin_unlock_bh(&sc->tx.txbuflock);

	return bf;
}

int get_jaldi_tx_from_skb(struct sk_buff *skb) {
	return 0; // TODO: decide on a packet format and implement this by reading protocol header field
}

int jaldi_pkt_type(struct jaldi_packet *pkt)
{
	// TODO : define format and check for potential control pkt
	return JALDI_DATA;
}

// TODO: should return appropriate type
int jaldi_get_qos_type_from_skb(struct sk_buff *skb) {
	return JALDI_QOS_BULK; // TODO: make this random-ish for testing
}


int jaldi_hw_ctl(struct jaldi_softc *sc, struct jaldi_packet *pkt) {
	// TODO: set the right control parameters

	return 0;
}

/* This method basically does the following:
 * - assigns skb to hw buffer
 * - sets up tx flags based on current config
 * - allocates DMA buffer
 * - 
 */
int jaldi_hw_tx(struct jaldi_softc *sc, struct jaldi_packet *pkt)
{
	struct jaldi_buf bf;
	struct jaldi_hw *hw;
	int r;

	DBG_START_MSG;

	hw = sc->hw;
	
	bf = jaldi_tx_get_buffer(sc);	
	if (!bf) {
		jaldi_print(JALDI_ALERT, "TX buffers are full\n");
		return -1;
	}

	r = jaldi_tx_setup_buffer(hw, bf, pkt);


	// TODO: set queue number in hw
	// bf.txq = softc.txq[qnum];

	// TODO: set config tx flags

	// setup buffer
	/* this is all out of date. see new xmit stuff.
	memset(&bf, 0, sizeof(struct jaldi_buf));

	jaldi_print(JALDI_DEBUG, WHERESTR, WHEREARG);
	bf.bf_dma_context = dma_map_single(sc->dev, pkt->skb->data, 
						pkt->skb->len, DMA_TO_DEVICE);

	jaldi_print(JALDI_DEBUG, WHERESTR, WHEREARG);
	if(unlikely(dma_mapping_error(sc->dev, bf.bf_dma_context))) {
		jaldi_print(JALDI_WARN, "dma_mapping_error during TX\n");
		return -ENOMEM;
	}

	jaldi_print(JALDI_DEBUG, WHERESTR, WHEREARG);
	bf.bf_buf_addr = bf.bf_dma_context;
	*/

	

/* Note to self: goal here is to get an sk_buff into a jaldi_buf, which encapsulates
 all the relevant /device/ specific information: dma stuff, etc. We need to couple this
 with a jaldi_tx_control which ensures the proper tx settings go with the sk_buff and push
 this to the device. 

 In short, the jaldi_buf represents an encapsulation of data that takes care of all the
 dma stuff. The jaldi_tx_control encapsulates all the tx settings we need: channel, power, the
 hardware queue we're on (WMA_BE or WMA_BK or WMA_VO). The sk_buff should really be in a 
 jaldi_packet, which will encapsulate the driver logic: which virtual queue we're on.

 This function should do everything needed to move a jaldi_packet into dma. The tx interrupt
 handler should drain the low level queues and initiate transmission over the air.
 */

	return 0;
}

//void jaldi_timer_tx(

/* 
 * This function receives an sk_buff from the kernel, and packages it up into a
 * jaldi_packet. Other high-level tx behaviors should be handled here. This 
 * should be completely device agnostic, as device-specific stuff should be
 * handled in jaldi_hw_tx.
 */
int jaldi_tx(struct sk_buff *skb, struct net_device *dev)
{
	int len, qnum;
	char *data;
	struct jaldi_softc *sc = netdev_priv(dev);
	struct jaldi_hw *hw;
	struct jaldi_packet *pkt;
	
	DBG_START_MSG;

	hw = sc->hw;

	data = skb->data;
	len = skb->len;
	
	dev->trans_start = jiffies;

	if(unlikely(hw->power_mode != JALDI_PM_AWAKE)) {
		jaldi_ps_wakeup(sc);
		jaldi_print(JALDI_DEBUG, "Waking up to do TX\n");
	}
	
	/* create jaldi packet */
	pkt = kmalloc (sizeof (struct jaldi_packet), GFP_KERNEL);
	if (!pkt) {
		jaldi_print(JALDI_WARN, "Out of memory while allocating packet\n");
		return 0;
	}
	
	pkt->next = NULL; /* XXX */
	pkt->sc = sc;
	pkt->data = skb->data;
	pkt->tx_time = get_jaldi_tx_from_skb(skb);
	pkt->qos_type = jaldi_get_qos_type_from_skb(skb);
	pkt->txq = &sc->tx.txq[qnum]; /* replaces need for txctl from ath9k */

	
	/* add jaldi packet to tx_queue */
	//jaldi_tx_enqueue(sc,pkt); // should add packet to proper software queue

	/* create kernel timer for packet transmission */
/*	struct timer_list tx_timer;
	init_timer(&tx_timer);
	tx_timer.function = jaldi_timer_tx;
	tx_timer.data = (unsigned long)*pkt;
	tx_timer.expires = pkt->tx_time;*/

	/* check for type: control packet or standard */
	if( jaldi_pkt_type(pkt) == JALDI_CONTROL ) { // should check header
		jaldi_hw_ctl(sc, pkt);
	} else {
		jaldi_hw_tx(sc, pkt);
	}
	
	return 0;

}

/* Enable rx interrupts */
static void jaldi_rx_ints(struct jaldi_softc *sc, int enable)
{
	DBG_START_MSG;
	sc->rx_int_enabled = enable;

}
/* Enable tx interrupts */
static void jaldi_tx_ints(struct jaldi_softc *sc, int enable)
{
	DBG_START_MSG;
	sc->tx_int_enabled = enable;
}

/* Set up a device's packet pool. (from snull) */
void jaldi_setup_pool(struct net_device *dev)
{
	return; /* TODO: Implement this */
}

/*
 * TODO: this is where ifconfig starts our driver. we need to integrate this 
 * with the rest of the driver. Should establish DMA, do the hw reset, all that
 * jazz. This should also be where we call whatever actually disables the acks
 * and carrier sense: that should not be done during HW initialization.
 */
int jaldi_open(struct net_device *dev)
{
	struct jaldi_softc *sc = netdev_priv(dev);
	struct jaldi_hw *hw = sc->hw;
	struct jaldi_channel *init_chan;
	int r;

	DBG_START_MSG;

	mutex_lock(&sc->mutex);

	/* set default channel if none specified */
	if (!hw->curchan) {
		init_chan = &sc->chans[JALDI_5GHZ][0]; /* default is chan 36 (5180Mhz, 14) */
		hw->curchan = init_chan;
	}

	memcpy(dev->dev_addr, "\0JALDI0", ETH_ALEN);

	spin_lock_bh(&sc->sc_resetlock);
	r = jaldi_hw_reset(hw, init_chan, true); /* we're settnig channel for first time so always true */
	if (r) {
		jaldi_print(JALDI_FATAL, "Unable to reset hw; reset status %d (freq %u MHz)\n",
				r, init_chan->center_freq);
		spin_unlock_bh(&sc->sc_resetlock);
		goto mutex_unlock;
	}
	spin_unlock_bh(&sc->sc_resetlock);

	netif_start_queue(dev);

mutex_unlock:
	mutex_unlock(&sc->mutex);

	return r;
}

void jaldi_cleanup(void)
{
	jaldi_ahb_exit();
	jaldi_pci_exit();
	return;
}

static const struct net_device_ops jaldi_netdev_ops =
{
	.ndo_open			= jaldi_open,
	.ndo_stop			= jaldi_release,
	.ndo_start_xmit			= jaldi_tx,
//	.ndo_get_stats			= jaldi_stats,
};

void jaldi_attach_netdev_ops(struct net_device *dev)
{
	dev->netdev_ops = &jaldi_netdev_ops;
}

int jaldi_init_module(void)
{
	int result, ret = -ENODEV;
	DBG_START_MSG;
	jaldi_print(JALDI_INFO, "Loading jaldimac. 2\n");

	result = jaldi_pci_init();
	if (result < 0) {
		jaldi_print(JALDI_FATAL, "No PCI device found, driver load cancelled.\n");
		goto err_pci;
	}

	jaldi_print(JALDI_DEBUG, "pci_init returns %d\n", result);

	result = jaldi_ahb_init();
	if (result < 0) {
		jaldi_print(JALDI_WARN, "AHB init failed (jaldimac devices should pass this!)\n");
		goto err_ahb;
	}
	jaldi_print(JALDI_DEBUG, "ahb_init returns %d\n", result);

	ret = 0;

	return ret;

err_ahb:
	jaldi_pci_exit();
err_pci:
	return ret;
}

module_init(jaldi_init_module);
module_exit(jaldi_cleanup);
