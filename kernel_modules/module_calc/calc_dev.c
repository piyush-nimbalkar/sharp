#include<linux/kernel.h>
#include<linux/module.h>
#include<asm/module.h>
#include<linux/fs.h>
#include "calc_dev.h"
#include<asm/uaccess.h>

#define SUCCESS 0

#define DEVICE_NAME "calc_dev"

static int dev_open = 0;

int ker_data[2]={0},data_count=0,ker_answer;

static int device_open(struct inode *inode,struct file *file)
{
  printk("\ndevice_open(%p)\n", file);
  if(dev_open)
    return -EBUSY;
  dev_open++;
  return SUCCESS;
}

static int device_release(struct inode *inode,struct file *file)
{
  printk("\ndevice_release(%p)\n",file);
  dev_open--;
  return 0;
}


static ssize_t device_read(struct file *file,int *data)
{
  put_user(ker_answer,data);
  printk("\n Inserted data %d to %d",ker_answer,*data); 
  return sizeof(data);
}



static ssize_t device_write(struct file *file,int *data)
{
  get_user(ker_data[data_count++%2],data);
  printk("\n Got data %d\n",ker_data[(data_count-1)%2]);
  return 1;
}

int device_ioctl(struct inode *inode,struct file *file,unsigned long ioctl_num,unsigned long ioctl_param)
{
    switch(ioctl_num)
    {
    case IOCTL_SET_DATA:
      printk("Enterd Input Case\n");
      device_write(file,(int *)ioctl_param);
      break;
      
    case IOCTL_GET_DATA:
      device_read(file,(int *)ioctl_param);
      break;
      
    case IOCTL_ADD_DATA:
      ker_answer=ker_data[0]+ker_data[1];
      printk("The addition is :%d",ker_answer);
      break;
      
    case IOCTL_SUB_DATA:
      ker_answer=ker_data[0]-ker_data[1];
      printk("The subtraction is :%d",ker_answer);
      break;
      
    case IOCTL_MUL_DATA:
      ker_answer=ker_data[0]*ker_data[1];
      printk("The multiplication is :%d",ker_answer);
      break;

    case IOCTL_DIV_DATA:
      if (ker_data[1]==0)
	{
	  printk("INVALID Operation ");
	  ker_answer=-9999;
	}
      else
	{
	  ker_answer=ker_data[0]/ker_data[1];
	  printk("The Answer is :%d",ker_answer);
	}
      break;

    default:
      printk("No case Executed");
    }
  return SUCCESS;
}

struct file_operations Fops = {
  .owner=THIS_MODULE,
  .read=device_read,
  .write=device_write,
  .llseek=NULL,
  .ioctl=device_ioctl,
  .open=device_open,
  .release=device_release
};


int init_module()
{
  int ret_val;
  ret_val=register_chrdev(MAJOR_NUM,DEVICE_NAME,&Fops);
  if(ret_val<0)
    {
      printk("Couldnt register module");
      return ret_val;
    }
  return 0;
}

void cleanup_module()
{
  unregister_chrdev(MAJOR_NUM,DEVICE_NAME);
}

//module_init(calc_open_module);
//module_exit(calc_close_module);



