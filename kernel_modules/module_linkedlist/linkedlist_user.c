#include<stdio.h>
#include<sys/ioctl.h>
#include"linkedlist_dev.h"

int main(void)
{
  int fd,choice;
  static int data;
  fd=open("/dev/"DEVICE_NAME,0);
  if(fd<0)
    {
      printf("main : Could not open the device");
      return -1;
    }
  do
    {
      printf("\n\nMenu\n\n1.Add\n2.Delete\n3.Display\n4.Exit\n\nYour choice : ");
      scanf("%d",&choice);
      switch(choice)
	{
	case 1: 
	  printf("Enter a value : ");
	  scanf("%d",&data);
	  ioctl(fd,IOCTL_LINKEDLIST_ADD,&data);
	  break;
	case 2:
	  printf("Enter the node to be deleted : ");
	  scanf("%d",&data);
	  ioctl(fd,IOCTL_LINKEDLIST_DEL,&data);
	  break;
	case 3:
	  printf("\nLinked List : ");
	  ioctl(fd,IOCTL_LINKEDLIST_SHOW,&data);
	  while(data!=-1)
	    {
	      printf(" %d ",data);
	      ioctl(fd,IOCTL_LINKEDLIST_SHOW,&data);
	    }
	  break;
	default:
	  return 0;
	}
    }while(1);
}
