obj-m += char_device_driver.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
	gcc read_from_device.c
	echo "1)create a node first. 2)a.out reads from device 3)use echo to write into device"
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	rm *.out
	rm *~


