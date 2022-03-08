#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/blk-mq.h>
#include <linux/hdreg.h>

#ifndef SECTOR_SIZE
#define SECTOR_SIZE 512
#endif

static int dev_major = 0;

//Просто внутреннее представление нашего блочного устройства
 //может содержать любые полезные данные
struct block_dev {
    sector_t capacity;
    u8 *data;  //Буфер данных для эмуляции реального запоминающего устройства
    struct blk_mq_tag_set tag_set;
    struct request_queue *queue;
    struct gendisk *gdisk;
};

// Экземпляр устройства
static struct block_dev *block_device = NULL;

static int blockdev_open(struct block_device *dev, fmode_t mode)
{
    printk("модуль загружен\n");

    return 0;
}

static void blockdev_release(struct gendisk *gdisk, fmode_t mode)
{
    printk("модуль освобожден\n");
}

int blockdev_ioctl(struct block_device *bdev, fmode_t mode, unsigned cmd, unsigned long arg)
{
    printk("ioctl cmd 0x%08x\n", cmd);

    return -ENOTTY;
}

/* Установить файл устройства блокировки I/O */
static struct block_device_operations blockdev_ops = {
    .owner = THIS_MODULE,
    .open = blockdev_open,
    .release = blockdev_release,
    .ioctl = blockdev_ioctl
};

//обслуживать запросы
static int do_request(struct request *rq, unsigned int *nr_bytes)
{
    int ret = 0;
    struct bio_vec bvec;
    struct req_iterator iter;
    struct block_dev *dev = rq->q->queuedata;
    loff_t pos = blk_rq_pos(rq) << SECTOR_SHIFT;
    loff_t dev_size = (loff_t)(dev->capacity << SECTOR_SHIFT);

    printk(KERN_WARNING "sblkdev: запрос начинается с сектора %lld  pos = %lld  dev_size = %lld\n", blk_rq_pos(rq), pos, dev_size);

    /* Выполнить итерацию по всем сегментам запросов */
    rq_for_each_segment(bvec, rq, iter)
    {
        unsigned long b_len = bvec.bv_len;

        /* Получить указатель на данные */
        void* b_buf = page_address(bvec.bv_page) + bvec.bv_offset;

        /* Простая проверка того, что мы не выходим за пределы памяти */
        if ((pos + b_len) > dev_size) {
            b_len = (unsigned long)(dev_size - pos);
        }

        if (rq_data_dir(rq) == WRITE) {
            /* Скопируйте данные в буфер в требуемую позицию */
            memcpy(dev->data + pos, b_buf, b_len);
        } else {
            /* Считывание данных из положения буфера */
            memcpy(b_buf, dev->data + pos, b_len);
        }

        /* Счетчики приращения */
        pos += b_len;
        *nr_bytes += b_len;
    }

    return ret;
}

/* функция обратного вызова очереди */
static blk_status_t queue_rq(struct blk_mq_hw_ctx *hctx, const struct blk_mq_queue_data* bd)
{
    unsigned int nr_bytes = 0;
    blk_status_t status = BLK_STS_OK;
    struct request *rq = bd->rq;

    /* Начать процедуру обслуживания запроса */
    blk_mq_start_request(rq);

    if (do_request(rq, &nr_bytes) != 0) {
        status = BLK_STS_IOERR;
    }

    /* Уведомлять ядро об обработанных nr_bytes */
    if (blk_update_request(rq, status, nr_bytes)) {
        /* Shouldn't fail */
        BUG();
    }

    /* Остановить процедуру подачи запроса */
    __blk_mq_end_request(rq, status);

    return status;
}

static struct blk_mq_ops mq_ops = {
    .queue_rq = queue_rq,
};

static int __init myblock_driver_init(void)
{
    /* Зарегистрируйте новое блочное устройство и получите основной номер устройства */
    dev_major = register_blkdev(dev_major, "testblk");

    block_device = kmalloc(sizeof (struct block_dev), GFP_KERNEL);

    if (block_device == NULL) {
        printk("Не удалось выделить память struct block_dev\n");
        unregister_blkdev(dev_major, "testblk");

        return -ENOMEM;
    }

    /* Установите емкость устройства */
    block_device->capacity = (204800); /* nsectors * SECTOR_SIZE; */
    /* Выделить соответствующий буфер данных */
    block_device->data = kmalloc(block_device->capacity, GFP_KERNEL);

    if (block_device->data == NULL) {
        printk("Не удалось выделить память device IO buffer\n");
        unregister_blkdev(dev_major, "testblk");
        kfree(block_device);

        return -ENOMEM;
    }

    printk("Initializing queue\n");

    block_device->queue = blk_mq_init_sq_queue(&block_device->tag_set, &mq_ops, 128, BLK_MQ_F_SHOULD_MERGE);

    if (block_device->queue == NULL) {
        printk("Не удалось выделить память device queue\n");
        kfree(block_device->data);

        unregister_blkdev(dev_major, "testblk");
        kfree(block_device);

        return -ENOMEM;
    }

   /* Установить структуру драйвера в качестве пользовательских данных очереди */
    block_device->queue->queuedata = block_device;

    /* Выделить новый диск */
    block_device->gdisk = alloc_disk(1);

    /* Установите все необходимые флаги и данные */
    block_device->gdisk->flags = GENHD_FL_NO_PART_SCAN;
    block_device->gdisk->major = dev_major;
    block_device->gdisk->first_minor = 0;

    block_device->gdisk->fops = &blockdev_ops;
    block_device->gdisk->queue = block_device->queue;
    block_device->gdisk->private_data = block_device;

    /* Задайте имя устройства так, как оно будет представлено в /dev */
    strncpy(block_device->gdisk->disk_name, "blockdrv\0", 9);

    printk("добавление диска %s\n", block_device->gdisk->disk_name);

    /* Установите емкость устройства */
    set_capacity(block_device->gdisk, block_device->capacity);

    /* Уведомлять ядро о новом дисковом устройстве */
    add_disk(block_device->gdisk);

    return 0;
}

static void __exit myblock_driver_exit(void)
{
    /* Не забудьте все очистить */
    if (block_device->gdisk) {
        del_gendisk(block_device->gdisk);
        put_disk(block_device->gdisk);
    }

    if (block_device->queue) {
        blk_cleanup_queue(block_device->queue);
    }

    kfree(block_device->data);

    unregister_blkdev(dev_major, "testblk");
    kfree(block_device);
}

module_init(myblock_driver_init);
module_exit(myblock_driver_exit);
MODULE_LICENSE("GPL");