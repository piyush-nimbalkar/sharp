#include<linux/module.h>
#include<linux/kernel.h>

int __init init_hello(void)
{
  printk(KERN_INFO "Hello World\n");
  return 0;
}

void __exit cleanup_hello(void)
{
  printk(KERN_INFO "Goodbye World\n");
}

module_init(init_hello);
module_exit(cleanup_hello);
