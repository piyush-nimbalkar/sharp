#include<linux/module.h>
#include<linux/kernel.h>
#include<linux/slab.h>

struct node
{
  int data;
  struct node * next;
}*start;

MODULE_LICENSE("GPL");

int init_module(void)
{
  int i;
  struct node *p;
  start=kmalloc(sizeof(struct node),GFP_KERNEL);
  p=start;
  start->data=0;
  for(i=1;i<5;i++)
    {
      p->next=kmalloc(sizeof(struct node),GFP_KERNEL);
      p=p->next;
      p->data=i;
    }
  p->next=NULL;
  printk("Linked List Created. Remove Module to view the Contents.");
  return 0;
}

void cleanup_module(void)
{
  struct node * p;
  p=start;
  while(p!=NULL)
    {
      printk("%d ",p->data);
      p=p->next;
    }
  p=start->next;
  while(p!=NULL)
    {
      kfree(start);
      start=p;
      p=p->next;
    }
}


