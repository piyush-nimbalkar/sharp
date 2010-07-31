SUBDIRS=char_device_driver module_calc module_hello module_linkedlist

all:
	@for i in $(SUBDIRS); do\
		cd $$i;\
		make;\
		cd ..;\
	done

clean:
	@for i in $(SUBDIRS); do\
		cd $$i;\
		make clean;\
		cd ..;\
	done