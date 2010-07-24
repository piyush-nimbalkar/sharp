#include<linux/module.h>
#include<linux/kernel.h>
#include<linux/fs.h>
#include<asm/uaccess.h>
#include "linkedlist_dev.h"

static int dev_open=0;

static node* head=NULL;
static node* ptr=NULL;

static int data;

static int device_open(struct inode* inode,struct file* file)
{
 if(dev_open)
    return -EBUSY;
 dev_open++;

 printk(KERN_INFO "%p device is open",file);
 return 0;
}

static int device_close(struct inode* inode,struct file* file)
{
  dev_open--;
  return 0;
}

static int device_read(struct inode *inode,struct file* file,int __user * buf)
{
  if(ptr!=NULL)  
    {
      put_user(ptr->data,buf);
      ptr=ptr->next;
    }
  else
    {
      put_user(-1,buf);
      ptr=head;
    }
  return 0;
}


static int device_write(struct inode *inode,struct file* file,char * buf)
{
  node* p=head;
  node* temp=(node*)kmalloc(sizeof(node),GFP_KERNEL);
  if(head==NULL)
    head=ptr=temp;
  else
    {
      while(p->next != NULL)
	p=p->next;
      p->next=temp;
    }
  temp->data=data;
  temp->next=NULL;
  return 0;
}

int device_ioctl(struct inode *inode,struct file *file,unsigned int ioctl_num,unsigned long ioctl_param) 
{
  node * p,* q;
  data=*(int *)ioctl_param;
  switch(ioctl_num)
    {
    case IOCTL_LINKEDLIST_ADD:
      device_write(inode,file,(char *)ioctl_param);
      break;
    case IOCTL_LINKEDLIST_DEL:
      if(head==NULL)
	printk(KERN_INFO "Element not found");
      else if(head->data==data)
	{
	  p=head;
	  head=head->next;
	  ptr=head;
	  kfree(p);
	}
      else if(head->next==NULL)
	printk(KERN_INFO "Element not found");
      else
	{
	  p=head;
	  q=p->next;
	  while(q->data!=data)
	    {
	      p=p->next;
	      if(q->next==NULL)
		{
		  printk(KERN_INFO "Element not found");
		  break;
		}
	      q=q->next;
	    }
	  if(q->data==data)
	    {
	      p->next=q->next;
	      kfree(q);
	    }
	}
      break;	  
      
    case IOCTL_LINKEDLIST_SHOW:
      if(head!=NULL)
	device_read(inode,file,(int *)ioctl_param);
      else
	put_user(-1,(int *)ioctl_param);
      break;
    }
  return 0;
}

struct file_operations fops = {
  .owner=THIS_MODULE,
  .read=device_read,
  .write=device_write,
  .llseek=NULL,
  .ioctl=device_ioctl,
  .open=device_open,
  .release=device_close
};


static int __init init_linkedlist()
{
  int ret_val=0;
  ret_val = register_chrdev(MAJOR_NO,DEVICE_NAME,&fops);
  if(ret_val<0)
    {
      printk(KERN_INFO "Sorry, cannot register !!");
      return ret_val;
    }
  printk(KERN_INFO "Registered Successfully !!");
  return 0;
}

static void __exit cleanup_linklist()
{
  node* p;
  printk(KERN_INFO "In cleanup");
  while(head!=NULL)
    {
      p=head->next;
      kfree(head);
      head=p;
    }
    unregister_chrdev(MAJOR_NO,DEVICE_NAME);    
}

module_init(init_linkedlist);
module_exit(cleanup_linklist);

