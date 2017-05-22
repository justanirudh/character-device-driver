#include <linux/module.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/types.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/semaphore.h>
#include <linux/ioctl.h>

#define CDRV_IOC_MAGIC 'k'
#define NUM 1
#define ASP_CLEAR_BUFF _IO(CDRV_IOC_MAGIC, NUM) 
#define MYDEV_NAME "mycdrv"

static size_t ramdisk_size = 16 * PAGE_SIZE; //default size. per device 'size' may vary after lseek
static unsigned int count = 1;
static struct class *foo_class;
static int NUM_DEVICES = 3; //default is 3 devices

module_param(NUM_DEVICES, int, S_IRUGO);

struct fake_device {
	struct semaphore sem; //adding fine grained sync in each method
	struct cdev my_cdev;
	char *ramdisk;
	int device_num;
	size_t size;
} *virtual_devices;

//inode is per device, *file is per open operation
static int mycdrv_open(struct inode *inode, struct file *file) { //syncing in open as well.
	struct fake_device *device; // device information

	pr_info(" attempting to open device: %s:\n", MYDEV_NAME); //alias for printk with flag as KERN_INFO
	pr_info(" MAJOR number = %d, MINOR number = %d\n", imajor(inode), iminor(inode));
	
	device = container_of(inode->i_cdev, struct fake_device, my_cdev);
	file->private_data = device; // for other methods

	pr_info("Successfully opened device: %s%d", MYDEV_NAME, device->device_num);

	return 0;
}

static int mycdrv_release(struct inode *inode, struct file *file) {//sync not required
	//dont free the pointer, since we want to save the state. write->read->write->read
	struct fake_device *device= file->private_data;
	pr_info(" CLOSING device: %s%d", MYDEV_NAME,device->device_num );
	return 0;
}

/*
 *For both methods, 
	'file' is the file pointer
	'lbuf' is the size of the requested data transfer. 
	'buf' argument points to the user buffer holding the data to be written or the empty buffer 
	where the newly read data should be placed.
	'ppos' is a pointer to a "long offset type" object that indicates the file position the user is accessing. 
	The return value is a "signed size type";the return value is the amount of memory still to be copied.0 if all
	went well. 
 *
 * */

//buf empty in this case
static ssize_t mycdrv_read(struct file *file, char __user * buf, size_t lbuf, loff_t * ppos) {
	int nbytes, maxbytes, bytes_to_do;

	struct fake_device* device = file->private_data;
	
	if(down_interruptible(&(device->sem))!=0){ //DONT change this to &sem
		printk(KERN_ALERT "mycdrv: could not lock device during read");
		return -1;
	}

	maxbytes = device->size - *ppos;
	bytes_to_do = maxbytes > lbuf ? lbuf : maxbytes;//bytes that are being transferred
	if (bytes_to_do == 0)
		pr_info("Reached end of the device on a read");
	nbytes = bytes_to_do - copy_to_user(buf, (device->ramdisk) + *ppos, bytes_to_do); //device to buff
	*ppos += nbytes;
	pr_info("\n Leaving the READ function, nbytes=%d, pos=%d\n",nbytes, (int)*ppos);
	
	up(&(device->sem));

	return nbytes;
}

//buf has stuff to be written to kernel
static ssize_t mycdrv_write(struct file *file, const char __user * buf, size_t lbuf, loff_t * ppos) {
	int nbytes, maxbytes, bytes_to_do;

	struct fake_device* device = file->private_data;

	if(down_interruptible(&(device->sem)) != 0){
		printk(KERN_ALERT "mycdrv: could not lock device during write");
		return -1;
	}

	maxbytes = device->size - *ppos; //ppos is present position of offset
	pr_info("maxbytes: %d, ramdisk size: %d, ppos:%d lbuf: %d", maxbytes,(int) device->size,(int) *ppos, (int) lbuf);
	bytes_to_do = maxbytes > lbuf ? lbuf : maxbytes;
	if (bytes_to_do == 0)
		pr_info("Reached end of the device on a write");
	nbytes = bytes_to_do - copy_from_user((device->ramdisk) + *ppos, buf, bytes_to_do); //buf to device
	*ppos += nbytes;
	pr_info("\n Leaving the WRITE function, nbytes=%d, pos=%d\n", nbytes, (int)*ppos);
	
	up(&(device->sem));

	return nbytes;
}

static loff_t mycdrv_lseek(struct file *file, loff_t offset, int orig) {
	loff_t testpos; //absolute offset from start of file pointer file

	struct fake_device* device = file->private_data;

	if(down_interruptible(&(device->sem)) != 0){
		printk(KERN_ALERT "mycdrv: could not lock device during lseek");
		return -1;
	}

	switch (orig) {
	case SEEK_SET:
		testpos = offset;
		break;
	case SEEK_CUR:
		testpos = file->f_pos + offset;
		break;
	case SEEK_END:
		testpos = device->size + offset;
		break;
	default:
		up(&(device->sem));
		return -EINVAL;
	}
	pr_info("size: %d, testpos: %d",(int)device->size,(int)testpos);

	if((int)testpos >(int)device->size){
		size_t diff = (size_t) testpos - device->size; //diff in memories
		char* new_location = kmalloc(device->size + diff, GFP_KERNEL); //new location
		memcpy(new_location, device->ramdisk, device->size); //copy stuff to new location
		kfree(device->ramdisk); //free up old memory
		device->ramdisk = new_location; //change pointer of ramdisk to new location
		memset((device->ramdisk) + device->size, 0, diff); //memset new (extra) memory to zeroes
		device->size = device->size + diff;//change size attr
		pr_info("Memory extended. New memory is %d bytes (Old memory was %d). ",(int)device->size,(int) (device->size - diff));
	}
	else{
		testpos = testpos >= 0 ? testpos : 0;
	}
	file->f_pos = testpos;//change file pointer
	pr_info("Seeking to pos=%ld\n", (long)testpos);
	
	up(&(device->sem));

	return testpos;
}

static long mycdrv_ioctl(struct file *file, unsigned int cmd, unsigned long arg){	
	struct fake_device* device = file->private_data;
	int err = 0;

	if(down_interruptible(&(device->sem)) != 0){
		printk(KERN_ALERT "mycdrv: could not lock device during ioctl");
		return -1;
	}

	if (_IOC_TYPE(cmd) != CDRV_IOC_MAGIC ){ 
		up(&(device->sem));
		return -ENOTTY;
	}

	if (_IOC_DIR(cmd) & _IOC_READ)
		err = !access_ok(VERIFY_WRITE, (void __user *)arg, _IOC_SIZE(cmd));
	else if (_IOC_DIR(cmd) & _IOC_WRITE)
		err =  !access_ok(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd));
	if (err){
		up(&(device->sem));
		return -EFAULT;
	}

	switch (cmd) {
  	case ASP_CLEAR_BUFF:
    	pr_info("caught the clearing buffer command!");
			memset(device->ramdisk, 0, device->size); //clear memory
			file->f_pos = 0; //reset file offset pointer to 0
      break;
    default:
      pr_info("Got unknown ioctl, CMD=%d", cmd);
			up(&(device->sem));
      return -EINVAL;	
	}	

	up(&(device->sem));

  return 0;
}

static const struct file_operations mycdrv_fops = {
	.owner = THIS_MODULE,
	.read = mycdrv_read,
	.write = mycdrv_write,
	.open = mycdrv_open,
	.release = mycdrv_release,
	.llseek = mycdrv_lseek,
	.unlocked_ioctl = mycdrv_ioctl
};

static int __init my_init(void) {
	int i;
	dev_t first_first,first_local;

	virtual_devices = kmalloc(NUM_DEVICES * sizeof(struct fake_device), GFP_KERNEL);
	foo_class = class_create(THIS_MODULE, "my_class"); //will be outside the device loop

	if (alloc_chrdev_region(&first_first, 0, NUM_DEVICES, MYDEV_NAME) < 0) { //dynamically allocate major number
			pr_err("failed to register character device region\n");
			return -1;
	}

	for(i = 0; i < NUM_DEVICES; ++i){

		first_local = MKDEV(MAJOR(first_first), i);
		
		cdev_init(&virtual_devices[i].my_cdev, &mycdrv_fops); //initialize cdev
		virtual_devices[i].ramdisk = kmalloc(ramdisk_size, GFP_KERNEL);
		memset(virtual_devices[i].ramdisk, 0, ramdisk_size ); //memsetting it to zeroes
		virtual_devices[i].size = ramdisk_size; 
		virtual_devices[i].my_cdev.owner = THIS_MODULE;
		virtual_devices[i].my_cdev.ops = &mycdrv_fops;
		virtual_devices[i].device_num = i;
		
		if (cdev_add(&virtual_devices[i].my_cdev, first_local, count) < 0) { //make it visible to kernel, also populates dev field of cdev
			pr_err("cdev_add() failed\n");
			cdev_del(&virtual_devices[i].my_cdev); //delete cdev
			unregister_chrdev_region(first_local, count);//unregister region
			kfree(virtual_devices[i].ramdisk);
			return -1;
		}

		device_create(foo_class, NULL, first_local, NULL, "%s%d", MYDEV_NAME, i);//dynamically creates entry in /dev

		pr_info("\nSucceeded in registering character device %s%d", MYDEV_NAME, i);
		pr_info("Major number: %d, Minor number: %d for this device", MAJOR(virtual_devices[i].my_cdev.dev ), MINOR(virtual_devices[i].my_cdev.dev ));
		sema_init(&virtual_devices[i].sem,1);
	}
	return 0;
}

static void __exit my_exit(void) { 	
	int i;
	dev_t first = virtual_devices[0].my_cdev.dev;
	for(i = 0; i < NUM_DEVICES; i++){ //destroy dynamically created devices
		device_destroy(foo_class, virtual_devices[i].my_cdev.dev);
	
		if (&virtual_devices[i].my_cdev)
			cdev_del(&virtual_devices[i].my_cdev); //delete the cdev

		kfree(virtual_devices[i].ramdisk); //free up ramdisk

		pr_info("device to be %s%d unregistered", MYDEV_NAME, i);

	}
	unregister_chrdev_region(first, NUM_DEVICES);//unregister the major/minor numbers
	pr_info("All devices unregistered");

	class_destroy(foo_class); //destroy dynamically created class
}

module_init(my_init);
module_exit(my_exit);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("user");
