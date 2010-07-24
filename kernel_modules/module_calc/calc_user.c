#include<fcntl.h>
#include<unistd.h>
#include<sys/ioctl.h>
#include<stdio.h>
#include<stdlib.h>
#include "calc_dev.h"
#include<string.h>

void check_err(int check)
{ 
  if(check<0)
    {
      printf("\nERROR Operation failed!!\n");
      exit(-1);
    }
}

main()
{
  int file_desc,ret_val,mydata[2],output,choice,ret;
  file_desc=open("/dev/"DEVICE_FILE_NAME,0);
  check_err(file_desc);
  do
    {
      printf("\nEnter two numbers\nNumber 1 : ");
  
      scanf("%d",mydata);
      ret=ioctl(file_desc,IOCTL_SET_DATA,mydata);
      check_err(ret);
  
      printf("Number 2 : ");
      scanf("%d",&mydata[1]);
  
      ret=ioctl(file_desc,IOCTL_SET_DATA,mydata+1);
      check_err(ret);
    
      printf("\n1.Addition\n2.Subtraction\n3.Multiplication\n4.Division\n5.Exit\n\nYour choice : ");
      scanf("%d",&choice);
      switch(choice)
	{ 
	case 1:
	  ret=ioctl(file_desc,IOCTL_ADD_DATA,&choice);
	  break;
	case 2:
	  ret=ioctl(file_desc,IOCTL_SUB_DATA,&choice);
	  break;
	case 3:
	  ret=ioctl(file_desc,IOCTL_MUL_DATA,&choice);
	  break;
	case 4:
	  ret=ioctl(file_desc,IOCTL_DIV_DATA,&choice);
	  break;
	default:
	  return 0;
	}
      check_err(ret);
  
      ret=ioctl(file_desc,IOCTL_GET_DATA,&output);
      check_err(ret);
      if(output==-9999)
	  printf("Error : Divide by Zero\n");
      else
	printf("\nAnswer : %d\n\n",output);
    }
  while(1);
  close(file_desc);
}
