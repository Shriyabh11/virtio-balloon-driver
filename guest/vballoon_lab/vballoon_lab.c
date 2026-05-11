#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/scatterlist.h>
#include <linux/sysinfo.h>
#include <linux/virtio.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_config.h>
#include <linux/jiffies.h>
#include <uapi/linux/virtio_balloon.h>

#define DRV_NAME "vballoon_lab"
#define ADJUST_BATCH 32

/*
 * Phase-3 starter knobs:
 * - pressure_enable: turn pressure-driven deflate on/off
 * - pressure_min_free_mb: if free RAM falls below this, prioritize deflate
 * - pressure_log_interval_ms: rate-limit pressure logs
 */
static bool pressure_enable = true;
module_param(pressure_enable, bool, 0644);
MODULE_PARM_DESC(pressure_enable, "Enable pressure-triggered deflate");

static unsigned int pressure_min_free_mb = 256;
module_param(pressure_min_free_mb, uint, 0644);
MODULE_PARM_DESC(pressure_min_free_mb, "Free RAM threshold (MB) to trigger pressure deflate");

static unsigned int pressure_log_interval_ms = 2000;
module_param(pressure_log_interval_ms, uint, 0644);
MODULE_PARM_DESC(pressure_log_interval_ms, "Min interval for pressure logs (ms)");

struct vb_page {
	struct page *page;
	__virtio32 pfn;
	struct list_head node;
};

struct vb_lab {
	struct virtio_device *vdev;
	struct virtqueue *inflate_vq;
	struct virtqueue *deflate_vq;

	struct task_struct *worker;
	wait_queue_head_t wq;
	spinlock_t lock;

	struct list_head ballooned;
	struct list_head inflight_inflate;
	struct list_head inflight_deflate;

	u32 actual_pages;
	u32 pending_inflate;
	u32 pending_deflate;
	bool stop;
	unsigned long last_pressure_log_jiffies;
};

static bool vb_under_pressure(void)
{
	struct sysinfo si;
	u64 free_kb;
	u64 threshold_kb;

	if (!pressure_enable)
		return false;

	si_meminfo(&si);
	free_kb = (u64)si.freeram * (PAGE_SIZE / 1024);
	threshold_kb = (u64)pressure_min_free_mb * 1024ULL;

	return free_kb < threshold_kb;
}

static u32 vb_read_target(struct vb_lab *vb)
{
	u32 target = 0;
	virtio_cread(vb->vdev, struct virtio_balloon_config, num_pages, &target);
	return target;
}

static void vb_write_actual(struct vb_lab *vb)
{
	u32 actual = vb->actual_pages;
	virtio_cwrite(vb->vdev, struct virtio_balloon_config, actual, &actual);
}

static u32 vb_effective_pages(struct vb_lab *vb)
{
	u32 eff;
	unsigned long flags;

	spin_lock_irqsave(&vb->lock, flags);
	eff = vb->actual_pages + vb->pending_inflate - vb->pending_deflate;
	spin_unlock_irqrestore(&vb->lock, flags);

	return eff;
}

static int vb_inflate_one(struct vb_lab *vb)
{
	struct vb_page *bp;
	struct scatterlist sg;
	unsigned long flags;
	int ret;

	bp = kzalloc(sizeof(*bp), GFP_KERNEL);
	if (!bp)
		return -ENOMEM;

	INIT_LIST_HEAD(&bp->node);

	bp->page = alloc_page(GFP_HIGHUSER | __GFP_NORETRY | __GFP_NOWARN);
	if (!bp->page) {
		kfree(bp);
		return -ENOMEM;
	}

	bp->pfn = cpu_to_virtio32(vb->vdev, page_to_pfn(bp->page));

	/* Put in inflight list BEFORE queueing to avoid callback race. */
	spin_lock_irqsave(&vb->lock, flags);
	list_add_tail(&bp->node, &vb->inflight_inflate);
	vb->pending_inflate++;
	spin_unlock_irqrestore(&vb->lock, flags);

	sg_init_one(&sg, &bp->pfn, sizeof(bp->pfn));
	ret = virtqueue_add_outbuf(vb->inflate_vq, &sg, 1, bp, GFP_KERNEL);
	if (ret) {
		spin_lock_irqsave(&vb->lock, flags);
		list_del_init(&bp->node);
		vb->pending_inflate--;
		spin_unlock_irqrestore(&vb->lock, flags);

		__free_page(bp->page);
		kfree(bp);
		return ret;
	}

	virtqueue_kick(vb->inflate_vq);
	return 0;
}
static int vb_deflate_one(struct vb_lab *vb)
{
	struct vb_page *bp;
	unsigned long flags;

	spin_lock_irqsave(&vb->lock, flags);
	if (list_empty(&vb->ballooned)) {
		spin_unlock_irqrestore(&vb->lock, flags);
		return -ENOENT;
	}
	bp = list_first_entry(&vb->ballooned, struct vb_page, node);
	list_del_init(&bp->node);
	spin_unlock_irqrestore(&vb->lock, flags);

	__free_page(bp->page);
	kfree(bp);

	spin_lock_irqsave(&vb->lock, flags);
	if (vb->actual_pages > 0)
		vb->actual_pages--;
	spin_unlock_irqrestore(&vb->lock, flags);

	vb_write_actual(vb);
	return 0;
}
static void vb_inflate_done(struct virtqueue *vq)
{
	struct vb_lab *vb = vq->vdev->priv;
	struct vb_page *bp;
	unsigned int len;
	unsigned long flags;
	bool changed = false;

	while ((bp = virtqueue_get_buf(vq, &len)) != NULL) {
		spin_lock_irqsave(&vb->lock, flags);
		list_del_init(&bp->node);
		list_add_tail(&bp->node, &vb->ballooned);
		vb->pending_inflate--;
		vb->actual_pages++;
		spin_unlock_irqrestore(&vb->lock, flags);
		changed = true;
	}

	if (changed)
		vb_write_actual(vb);

	wake_up(&vb->wq);
}

static void vb_deflate_done(struct virtqueue *vq)
{
	struct vb_lab *vb = vq->vdev->priv;
	struct vb_page *bp;
	unsigned int len;
	unsigned long flags;
	bool changed = false;

	while ((bp = virtqueue_get_buf(vq, &len)) != NULL) {
		/* Safe even if not on inflight list */
		spin_lock_irqsave(&vb->lock, flags);
		if (!list_empty(&bp->node))
			list_del_init(&bp->node);
		if (vb->pending_deflate > 0)
			vb->pending_deflate--;
		if (vb->actual_pages > 0)
			vb->actual_pages--;
		spin_unlock_irqrestore(&vb->lock, flags);

		__free_page(bp->page);
		kfree(bp);
		changed = true;
	}

	if (changed)
		vb_write_actual(vb);

	wake_up(&vb->wq);
}

static int vb_worker(void *arg)
{
	struct vb_lab *vb = arg;

	while (!kthread_should_stop()) {
		u32 target = vb_read_target(vb);
		u32 eff = vb_effective_pages(vb);
		unsigned long flags;
		u32 pending_inflate;
		int progress = 0;
		bool pressure = vb_under_pressure();

		spin_lock_irqsave(&vb->lock, flags);
		pending_inflate = vb->pending_inflate;
		spin_unlock_irqrestore(&vb->lock, flags);

		/*
		 * Under pressure, we prioritize returning memory to guest first,
		 * even if host target asks for a larger balloon.
		 */
		if (pressure) {
			if (!vb_deflate_one(vb)) {
				progress = 1;
				if (time_after(jiffies,
					       vb->last_pressure_log_jiffies +
					       msecs_to_jiffies(pressure_log_interval_ms))) {
					pr_info(DRV_NAME ": pressure deflate (free < %u MB)\n",
						pressure_min_free_mb);
					vb->last_pressure_log_jiffies = jiffies;
				}
			}
		} else if (eff < target) {
			if (!pending_inflate && !vb_inflate_one(vb))
				progress = 1;
		} else if (eff > target) {
			if (!vb_deflate_one(vb))
				progress = 1;
		}

		if (!progress)
			wait_event_interruptible_timeout(vb->wq,
							 kthread_should_stop(),
							 msecs_to_jiffies(200));
		else
			cond_resched();
	}

	return 0;
}
static void vb_config_changed(struct virtio_device *vdev)
{
	struct vb_lab *vb = vdev->priv;
	wake_up(&vb->wq);
}

static void vb_free_list(struct list_head *head)
{
	struct vb_page *bp, *tmp;

	list_for_each_entry_safe(bp, tmp, head, node) {
		list_del(&bp->node);
		if (bp->page)
			__free_page(bp->page);
		kfree(bp);
	}
}

static int vb_probe(struct virtio_device *vdev)
{
	struct vb_lab *vb;
	struct virtqueue *vqs[2];
	vq_callback_t *callbacks[] = { vb_inflate_done, vb_deflate_done };
	const char * const names[] = { "inflate", "deflate" };
	int ret;

	vb = kzalloc(sizeof(*vb), GFP_KERNEL);
	if (!vb)
		return -ENOMEM;

	vb->vdev = vdev;
	vdev->priv = vb;
	spin_lock_init(&vb->lock);
	init_waitqueue_head(&vb->wq);
	INIT_LIST_HEAD(&vb->ballooned);
	INIT_LIST_HEAD(&vb->inflight_inflate);
	INIT_LIST_HEAD(&vb->inflight_deflate);

	ret = virtio_find_vqs(vdev, 2, vqs, callbacks, names, NULL);
	if (ret)
		goto err_free;

	vb->inflate_vq = vqs[0];
	vb->deflate_vq = vqs[1];

	virtio_device_ready(vdev);

	vb->worker = kthread_run(vb_worker, vb, "vballoon_lab");
	if (IS_ERR(vb->worker)) {
		ret = PTR_ERR(vb->worker);
		goto err_del_vqs;
	}

	pr_info(DRV_NAME ": probe ok\n");
	return 0;

err_del_vqs:
	vdev->config->del_vqs(vdev);
err_free:
	kfree(vb);
	return ret;
}
static void vb_remove(struct virtio_device *vdev)
{
	struct vb_lab *vb = vdev->priv;

	if (!vb)
		return;

	if (vb->worker)
		kthread_stop(vb->worker);

	virtio_reset_device(vdev);
	vdev->config->del_vqs(vdev);

	vb_free_list(&vb->ballooned);
	vb_free_list(&vb->inflight_inflate);
	vb_free_list(&vb->inflight_deflate);

	kfree(vb);
	pr_info(DRV_NAME ": removed\n");
}

static const struct virtio_device_id vb_id_table[] = {
	{ VIRTIO_ID_BALLOON, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static struct virtio_driver vb_driver = {
	.feature_table = NULL,
	.feature_table_size = 0,
	.driver.name = DRV_NAME,
	.driver.owner = THIS_MODULE,
	.id_table = vb_id_table,
	.probe = vb_probe,
	.remove = vb_remove,
	.config_changed = vb_config_changed,
};

module_virtio_driver(vb_driver);

MODULE_DESCRIPTION("Virtio balloon lab driver with pressure-aware memory management");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shriya Bharadwaj");
