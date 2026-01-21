/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include "aesd-circular-buffer.h"
#include "aesdchar.h"

int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("leekoei");
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");
    /**
     * TODO: handle open
     */
    struct aesd_dev *dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    
    filp->private_data = dev;

    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    /**
     * TODO: handle release
     */
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);
    /**
     * TODO: handle read
     */
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry;
    size_t bytes_to_copy, entry_offset;

    mutex_lock(&dev->lock);
    entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->buffer, *f_pos, &entry_offset);
    if (entry == NULL) {
        mutex_unlock(&dev->lock);
        return 0;
    }

    bytes_to_copy = entry->size - entry_offset;
    if (bytes_to_copy > count) {
        bytes_to_copy = count;
    }

    if (copy_to_user(buf, entry->buffptr + entry_offset, bytes_to_copy)) {
        mutex_unlock(&dev->lock);
        return -EFAULT;
    }

    *f_pos += bytes_to_copy;
    mutex_unlock(&dev->lock);

    return bytes_to_copy;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = -ENOMEM;
    struct aesd_dev *dev = filp->private_data;
    size_t size = dev->entry.size;
    size_t bytes_to_copy = 0;

    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);
    /**
     * TODO: handle write
     */

    mutex_lock(&dev->lock);
    if (size == 0) {
        /* The case when the current entry is empty */
        dev->entry.buffptr = kmalloc(count, GFP_KERNEL);
    } else {
        /* The case when the current entry is pending more data */
        int size_new = size + count;
        dev->entry.buffptr = krealloc((void *)dev->entry.buffptr, size_new, GFP_KERNEL);
    }

    if (dev->entry.buffptr == NULL) {
        mutex_unlock(&dev->lock);
        return retval;
    }

    bytes_to_copy = copy_from_user((char *)dev->entry.buffptr + size, buf, count);
    retval = count - bytes_to_copy;
    dev->entry.size += retval;

    if (buf[count - 1] == '\n') {
        /* Add the entry to the circular buffer */
        const char *overwritten_buffptr = aesd_circular_buffer_add_entry(&dev->buffer, &dev->entry);
        if (overwritten_buffptr != NULL) {
            kfree((void *)overwritten_buffptr);
        }

        /* Reset the current entry */
        dev->entry.buffptr = NULL;
        dev->entry.size = 0;
    }

    mutex_unlock(&dev->lock);
    return retval;
}

struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}

int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    /**
     * TODO: initialize the AESD specific portion of the device
     */
    /* Initialize the mutex */
    mutex_init(&aesd_device.lock);

    /* Initialize the circular buffer */
    aesd_circular_buffer_init(&aesd_device.buffer);

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    uint8_t index;
    struct aesd_buffer_entry *entryptr;

    dev_t devno = MKDEV(aesd_major, aesd_minor);

    cdev_del(&aesd_device.cdev);

    /**
     * TODO: cleanup AESD specific poritions here as necessary
     */
    /* Free the circular buffer entries */
    AESD_CIRCULAR_BUFFER_FOREACH(entryptr,&aesd_device.buffer,index) {
        kfree(entryptr->buffptr);
    }

    kfree(aesd_device.entry.buffptr);

    mutex_destroy(&aesd_device.lock);

    unregister_chrdev_region(devno, 1);
}

module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
