#include<linux/module.h>
#include<linux/kernel.h>
#include<linux/fs.h>
#include<asm/uaccess.h>
#include"char_device_driver.h"
#include<linux/cdev.h>

#define BUFFER_LENGTH 100

static int device_open_count=0;
static char buffer[BUFFER_LENGTH];

static int device_open(struct inode * inode,struct file * file)
{
  if(device_open_count>0)
    {
      return -EBUSY;
    }
  else
    device_open_count++;
  printk(KERN_INFO "Char_dev : Device opened successfully.");
  return SUCCESS;
}

static int device_release(struct inode * inode,struct file * file)
{
  if(device_open_count>0)
    {
      device_open_count--;
      return SUCCESS;
    }
  printk(KERN_INFO "Char_dev : Device Released Successfully.");
  return -1;
}

static ssize_t device_read(struct file * file,char __user * user_buffer,size_t length,loff_t * offp)
{
  int bytes_read=0;
  char * temp_buf=buffer;
  printk(KERN_INFO "Char_dev : Read");
  if(*temp_buf==0)
    return 0;
  while(length && *temp_buf)
    {
      put_user(*(temp_buf++),user_buffer++);
      length--;
      bytes_read++;
    }
  put_user(0,user_buffer++);
  return bytes_read;
}

static ssize_t device_write(struct file * file,const char __user * user_buffer,size_t length,loff_t * offp)
{
  char * temp_buf=buffer;
  int bytes_written=0;
  printk(KERN_INFO "Char_dev : Write");
  while(length)
    {
      get_user(*(temp_buf++),user_buffer++);
      length--;
      bytes_written++;
    }
  *temp_buf=0;
  return bytes_written;
}

static int device_ioctl(struct inode * inode,struct file * file,unsigned int ioctl_no,unsigned long ioctl_param)
{
  printk(KERN_INFO "char_dev : IOCTL");
  switch(ioctl_no)
    {
    case IOCTL_WRITE:device_write(file,(char *)ioctl_param,1,NULL);
      break;
    }
  return SUCCESS;
}

struct file_operations fops={
  .owner=THIS_MODULE,
  .read=device_read,
  .write=device_write,
  .llseek=NULL,
  .open=device_open,
  .ioctl=device_ioctl,
  .release=device_release
};

int init_module(void)
{
  int ret=register_chrdev(MAJOR_NUM,"char_dev",&fops);
  if(ret<0)
      printk(KERN_INFO "Char_Dev : Could not register.");
  else
    printk(KERN_INFO "Char_dev : Registered successfully.");

  return SUCCESS;
}

void cleanup_module(void)
{
  unregister_chrdev(MAJOR_NUM,"char_dev");
}


