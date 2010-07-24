#ifndef CHARDEV_H
#define CHARDEV_H
#endif

#define MAJOR_NUM 100

#include<linux/ioctl.h>

#define IOCTL_SET_DATA _IOR(MAJOR_NUM,0,int*)
#define IOCTL_GET_DATA _IOR(MAJOR_NUM,1,int*)
#define IOCTL_ADD_DATA _IOR(MAJOR_NUM,2,int*)
#define IOCTL_SUB_DATA _IOR(MAJOR_NUM,3,int*)
#define IOCTL_MUL_DATA _IOR(MAJOR_NUM,4,int*)
#define IOCTL_DIV_DATA _IOR(MAJOR_NUM,5,int*)
#define DEVICE_FILE_NAME "calc_dev"

