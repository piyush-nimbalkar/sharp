obj-m += calc_dev.o
CC=gcc
FILE=calc_user.c
RM=rm ./a.out
all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
	$(CC) $(FILE)
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	$(RM)