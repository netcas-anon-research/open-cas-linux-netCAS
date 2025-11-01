
        static inline int cas_alloc_mq_disk(struct gendisk **gd, struct request_queue **queue,
					    struct blk_mq_tag_set *tag_set)
        {
		*gd = alloc_disk(1);
		if (!(*gd))
			return -ENOMEM;

		*queue = blk_mq_init_queue(tag_set);
		if (IS_ERR_OR_NULL(*queue)) {
			put_disk(*gd);
			return -ENOMEM;
		}
		(*gd)->queue = *queue;

		return 0;
        }

	static inline void cas_cleanup_mq_disk(struct gendisk *gd)
    {
		blk_cleanup_queue(gd->queue);
		gd->queue = NULL;
		put_disk(gd);
	}
#define cas_blk_rq_append_bio(rq, bounce_bio) \
            blk_rq_append_bio(rq, &bounce_bio)

    static inline bool cas_bdev_exist(const char *path)
    {
        struct block_device *bdev;

        bdev = lookup_bdev(path);
        if (IS_ERR(bdev))
            return false;
        bdput(bdev);
        return true;
    }

    static inline bool cas_bdev_match(const char *path, struct block_device *bd)
    {
        struct block_device *bdev;
        bool match = false;

        bdev = lookup_bdev(path);
        if (IS_ERR(bdev))
            return false;
        match = (bdev == bd);
        bdput(bdev);
        return match;
    }
#define cas_bdev_nr_sectors(bd) \
            (bd->bd_part->nr_sects)
#define cas_bdev_whole(bd) \
            (bd->bd_contains)

	static inline int cas_bd_get_next_part(struct block_device *bd)
	{
		int part_no = 0;
		struct gendisk *disk = bd->bd_disk;
		struct disk_part_iter piter;
		struct hd_struct *part;

		mutex_lock(&bd->bd_mutex);

		disk_part_iter_init(&piter, disk, DISK_PITER_INCL_EMPTY);
		while ((part = disk_part_iter_next(&piter))) {
			part_no = part->partno;
			break;
		}
		disk_part_iter_exit(&piter);

		mutex_unlock(&bd->bd_mutex);

		return part_no;
	}

	static inline int cas_blk_get_part_count(struct block_device *bdev)
	{
		struct disk_part_tbl *ptbl;
		int i, count = 0;

		rcu_read_lock();
		ptbl = rcu_dereference(bdev->bd_disk->part_tbl);
		for (i = 0; i < ptbl->len; ++i) {
			if (rcu_access_pointer(ptbl->part[i]))
				count++;
		}
		rcu_read_unlock();

		return count;
	}
static inline struct bio *cas_bio_clone(struct bio *bio, gfp_t gfp_mask)
            {
                return bio_clone_fast(bio, gfp_mask, NULL);
            }
#define CAS_BIO_SET_DEV(bio, bdev) \
            bio_set_dev(bio, bdev)
#define CAS_BIO_GET_DEV(bio) \
            bio->bi_disk
#define CAS_IS_DISCARD(bio) \
			(((CAS_BIO_OP_FLAGS(bio)) & REQ_OP_MASK) == REQ_OP_DISCARD)
#define CAS_BIO_DISCARD \
			((REQ_OP_WRITE | REQ_OP_DISCARD))
#define CAS_BIO_OP_STATUS(bio) \
			bio->bi_status
#define CAS_BIO_OP_FLAGS_FORMAT "0x%016X"
#define CAS_BIO_OP_FLAGS(bio) \
			(bio)->bi_opf
#define CAS_BIO_GET_GENDISK(bio) (bio->bi_disk)
#define CAS_BIO_BISIZE(bio) \
			bio->bi_iter.bi_size
#define CAS_BIO_BIIDX(bio) \
			bio->bi_iter.bi_idx
#define CAS_BIO_BISECTOR(bio) \
			bio->bi_iter.bi_sector
#define CAS_BIO_MAX_VECS ((uint32_t)BIO_MAX_PAGES)

    static inline struct bio *cas_bio_split(struct bio *bio, int sectors)
    {
        return bio_split(bio, sectors, GFP_NOIO, &fs_bio_set);
    }
#define CAS_SEGMENT_BVEC(vec) \
			(&(vec))
#define CAS_END_REQUEST_ALL blk_mq_end_request
#define cas_blk_queue_exit(q) 
#define CAS_BLK_STATUS_T blk_status_t
#define CAS_BLK_STS_NOTSUPP BLK_STS_NOTSUPP
#define CAS_DAEMONIZE(name, arg...) \
			do { } while (0)
#define CAS_ALIAS_NODE_TO_DENTRY(alias) \
			container_of(alias, struct dentry, d_u.d_alias)
#define CAS_SET_DISCARD_ZEROES_DATA(queue_limits, val) \
			({})
#define CAS_ERRNO_TO_BLK_STS(status) errno_to_blk_status(status)
#define CAS_IS_SET_FLUSH(flags) \
            ((flags) & REQ_PREFLUSH)
#define CAS_SET_FLUSH(flags) \
            ((flags) | REQ_PREFLUSH)
#define CAS_CLEAR_FLUSH(flags) \
            ((flags) & ~REQ_PREFLUSH)

        static inline unsigned long cas_global_zone_page_state(enum zone_stat_item item)
        {
            return global_zone_page_state(item);
        }
#define CAS_ALIAS_NODE_TYPE \
			struct hlist_node
#define CAS_DENTRY_LIST_EMPTY(head) \
			hlist_empty(head)
#define CAS_INODE_FOR_EACH_DENTRY(pos, head) \
			hlist_for_each(pos, head)
#define CAS_FILE_INODE(file) \
			file->f_inode

        static inline void cas_blk_queue_make_request(struct request_queue *q,
                make_request_fn *mfn)
        {
            blk_queue_make_request(q, mfn);
        }
#define MODULE_MUTEX_SUPPORTED 1
#define CAS_MODULE_PUT_AND_EXIT(code) module_put_and_exit(code)
#define CAS_BLK_MQ_F_STACKING 0
#define CAS_BLK_MQ_F_BLOCKING \
            BLK_MQ_F_BLOCKING

#include <uapi/asm-generic/mman-common.h>
#include <uapi/linux/mman.h>
	static inline unsigned long cas_vm_mmap(struct file *file,
			unsigned long addr, unsigned long len)
	{
		return vm_mmap(file, addr, len, PROT_READ | PROT_WRITE,
			MAP_ANONYMOUS | MAP_PRIVATE, 0);
	}

	static inline int cas_vm_munmap(unsigned long start, size_t len)
	{
		return vm_munmap(start, len);
	}
#define cas_blk_queue_bounce(q, bounce_bio) \
			({})
#define CAS_SET_QUEUE_CHUNK_SECTORS(queue, chunk_size) \
			queue->limits.chunk_sectors = chunk_size
#define CAS_QUEUE_FLAG_SET(flag, request_queue) \
			blk_queue_flag_set(flag, request_queue)

	static inline void cas_copy_queue_limits(struct request_queue *exp_q,
			struct request_queue *cache_q, struct request_queue *core_q)
	{
		exp_q->limits = cache_q->limits;
		exp_q->limits.max_sectors = core_q->limits.max_sectors;
		exp_q->limits.max_hw_sectors = core_q->limits.max_hw_sectors;
		exp_q->limits.max_segments = core_q->limits.max_segments;
		exp_q->limits.max_write_same_sectors = 0;
		exp_q->limits.max_write_zeroes_sectors = 0;
	}
#define CAS_QUEUE_SPIN_LOCK(q) spin_lock_irq(&q->queue_lock)
#define CAS_QUEUE_SPIN_UNLOCK(q) spin_unlock_irq(&q->queue_lock)

        static inline void cas_reread_partitions(struct block_device *bdev)
        {
            ioctl_by_bdev(bdev, BLKRRPART, (unsigned long)NULL);
        }
#define CAS_SET_SUBMIT_BIO(_fn)

	static inline blk_qc_t cas_submit_bio(int rw, struct bio *bio)
	{
		CAS_BIO_OP_FLAGS(bio) |= rw;
		return submit_bio(bio);
	}
#define CAS_GET_CURRENT_TIME(timespec) ktime_get_real_ts64(timespec)

        static inline int cas_vfs_ioctl(struct file *file, unsigned int cmd,
                unsigned long arg)
        {
            return vfs_ioctl(file, cmd, arg);
        }

        static inline void *cas_vmalloc(unsigned long size, gfp_t gfp_mask)
        {
            return __vmalloc(size, gfp_mask, PAGE_KERNEL);
        }
#define CAS_WLTH_SUPPORT \
			1
#define CAS_CHECK_BARRIER(bio) \
			((CAS_BIO_OP_FLAGS(bio) & RQF_SOFTBARRIER) != 0)
#define CAS_REFER_BLOCK_CALLBACK(name) \
				   name##_callback
#define CAS_BLOCK_CALLBACK_INIT(BIO) \
			{; }
#define CAS_BLOCK_CALLBACK_RETURN(BIO) \
			{ return; }
#define CAS_BIO_ENDIO(BIO, BYTES_DONE, ERROR) \
			({ CAS_BIO_OP_STATUS(BIO) = ERROR; bio_endio(BIO); })
#define CAS_DECLARE_BLOCK_CALLBACK(name, BIO, BYTES_DONE, ERROR) \
			void name##_callback(BIO)
#define CAS_BLOCK_CALLBACK_ERROR(BIO, ERROR) \
			CAS_BIO_OP_STATUS(BIO)

        static inline unsigned long long cas_generic_start_io_acct(
                struct bio *bio)
        {
            struct gendisk *gd = CAS_BIO_GET_DEV(bio);

            generic_start_io_acct(gd->queue, bio_data_dir(bio),
                    bio_sectors(bio), &gd->part0);
            return jiffies;
        }

        static inline void cas_generic_end_io_acct(
                struct bio *bio, unsigned long start_time)
        {
            struct gendisk *gd = CAS_BIO_GET_DEV(bio);

            generic_end_io_acct(gd->queue, bio_data_dir(bio),
                    &gd->part0, start_time);
        }
#define CAS_CHECK_QUEUE_FLUSH(q) \
			test_bit(QUEUE_FLAG_WC, &(q)->queue_flags)
#define CAS_CHECK_QUEUE_FUA(q) \
			test_bit(QUEUE_FLAG_FUA, &(q)->queue_flags)

	static inline void cas_set_queue_flush_fua(struct request_queue *q,
			bool flush, bool fua)
	{
		blk_queue_write_cache(q, flush, fua);
	}
