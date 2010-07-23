#include<linux/module.h>
#include<linux/kernel.h>

int init_module(void)
{
  int fd;
  fd=open("/dev/stdin",1);
  printk(KERN_INFO "Hello World\n");
  return 0;
}

void cleanup_module(void)
{
  printk(KERN_INFO "Goodbye World\n");
}

//module_init(init_module_123);
//module_clean(cleanup_module_123);
