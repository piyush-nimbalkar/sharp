#ifndef _CHAR_DEV_H_
#define _CHAR_DEV_H_

#include<linux/ioctl.h>

#define IOCTL_WRITE _IOR(MAJOR_NUM,0,char *)
#define MAJOR_NUM 100
#define DEVICE_NAME "char_dev"
#define SUCCESS 0

#endif

