#ifndef LINKEDLIST_DEV_H
#define LINKEDLIST_DEV_H

#define MAJOR_NO 100

typedef struct node
{
  int data;
  struct node* next;
}node;


#define IOCTL_LINKEDLIST_ADD _IOR(MAJOR_NO,0,int)

#define IOCTL_LINKEDLIST_DEL _IOR(MAJOR_NO,1,int)

#define IOCTL_LINKEDLIST_SHOW _IOR(MAJOR_NO,2,int)

#define DEVICE_NAME "linkedlist_dev"

#endif



