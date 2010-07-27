#include<stdio.h>
#include"char_device_driver.h"

int main(void)
{
  int fd=open("/dev/"DEVICE_NAME,0),bytes_read=0;
  char buff[100];
  if(fd<0)
    {
      printf("read_from_device : can't load device\n");
      return -1;
    }
  bytes_read=read(fd,buff,100);
  printf("\nData Read : %s\nBytes read : %d\n",buff,bytes_read);
  return 0;
}


