#include<linux/module.h>
#include<linux/kernel.h>
#include<linux/moduleparam.h>

static int no1 = 0;
static int no2 = 1;

module_param(no1,int,0);
module_param(no2,int,0);

int init_module(void)
{
  printk(KERN_INFO "Hello World !\n");
  printk(KERN_INFO "Sum = %d\n",no1 + no2);
  return 0;
}

void cleanup_module(void)
{
  printk(KERN_INFO "Goodbye World !\n");
}
