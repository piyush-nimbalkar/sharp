obj-m += linkedlist_dev.o
CC=gcc
FILE=linkedlist_user.c

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
	$(CC) $(FILE)

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
