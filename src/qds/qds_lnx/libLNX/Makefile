.PHONY : clean

LIBUDEV_LIBS = /lib/x86_64-linux-gnu/libudev.so.1
LIBUDEV_AARCH64_LIBS = /lib/aarch64-linux-gnu/libudev.so.1
LIBUSB_REDHAT_LIBS = /usr/lib/systemd/libsystemd-shared-239.so
LIBUSB_SOURCE = ../../libusb/source/qcom-libusb.cpp
LIBUSB_HEADER = ../../libusb/source/qcom-libusb.h
INCLUDE_FLAGS = ../inc
COMMON_SOURCE = ../../common/*.cpp
INCLUDE_COMMON = ../../common
LIBUSB_LOADER = ../../libusb/source/libusb-dynamic-loader.cpp
LIBUSB_INCLUDE = $(shell pkg-config --cflags libusb-1.0)
WARNINGS = -Wall -Wextra -std=c++11
CFLAGS = $(WARNINGS)
LDFLAGS = $(LIBUDEV_LIBS)
OS_RELEASE = $(shell cat /etc/os-release | grep PRETTY_NAME)
ARCH := $(shell uname -m)

release: clean
ifeq ($(ARCH),aarch64)
	@echo "Making libqcdev (release aarch64)"
	@g++ -c qcdev.cpp QdIoQmi.cpp qdaio.cpp $(LIBUSB_SOURCE) $(LIBUSB_LOADER) $(COMMON_SOURCE) -fPIC -I $(LIBUSB_HEADER) -I $(INCLUDE_FLAGS) -I $(INCLUDE_COMMON) $(LIBUSB_INCLUDE) $(CFLAGS) -fpermissive -w -D QCDEV_LOAD_LIBRARY
	@g++ -o libqcdev.so -shared *.o $(LIBUDEV_AARCH64_LIBS) -g -lpthread -ldl -w
	@ar rcs libDeviceDiscovery.a *.o
else ifneq (,$(findstring Red Hat Enterprise Linux,$(OS_RELEASE)))
	@echo "Making libqcdev (release RHEL)"
	@g++ -c qcdev.cpp QdIoQmi.cpp qdaio.cpp $(COMMON_SOURCE) -fPIC -I $(INCLUDE_FLAGS) -I $(INCLUDE_COMMON) $(LIBUSB_INCLUDE) $(CFLAGS) -fpermissive -w -D QCDEV_LOAD_LIBRARY
	@g++ -o libqcdev.so -shared *.o $(LIBUSB_REDHAT_LIBS) -g -lpthread -ldl -w
	@ar rcs libDeviceDiscovery.a *.o
else
	@echo "Making libqcdev (release x86_64)"
	@g++ -c qcdev.cpp QdIoQmi.cpp qdaio.cpp $(LIBUSB_SOURCE) $(LIBUSB_LOADER) $(COMMON_SOURCE) -fPIC -I $(LIBUSB_HEADER) -I $(INCLUDE_FLAGS) -I $(INCLUDE_COMMON) $(LIBUSB_INCLUDE) $(CFLAGS) -fpermissive -w -D QCDEV_LOAD_LIBRARY
	@g++ -o libqcdev.so -shared *.o $(LIBUDEV_LIBS) -lpthread -ldl -w
	@ar rcs libDeviceDiscovery.a *.o
endif
	@echo "libqcdev.so Created"
	@rm -f *.o

debug: clean
	@echo "Making libqcdev (debug)"
ifneq (,$(findstring Red Hat Enterprise Linux,$(OS_RELEASE)))
	@g++ -c -g qcdev.cpp QdIoQmi.cpp qdaio.cpp $(COMMON_SOURCE) -fPIC -I $(INCLUDE_FLAGS) -I $(INCLUDE_COMMON) $(LIBUSB_INCLUDE) $(CFLAGS) -fpermissive -w -D QCDEV_LOAD_LIBRARY
	@g++ -o libqcdev.so -shared *.o $(LIBUSB_REDHAT_LIBS) -g -lpthread -ldl -w
	@ar rcs libDeviceDiscovery.a *.o
else
	@g++ -c -g qcdev.cpp QdIoQmi.cpp qdaio.cpp $(LIBUSB_SOURCE) $(LIBUSB_LOADER) $(COMMON_SOURCE) -fPIC -I $(LIBUSB_HEADER) -I $(INCLUDE_FLAGS) -I $(INCLUDE_COMMON) $(LIBUSB_INCLUDE) $(CFLAGS) -fpermissive -w -D QCDEV_LOAD_LIBRARY
	@g++ -o libqcdev.so -shared *.o $(LIBUDEV_LIBS) -g -lpthread -ldl -w
	@ar rcs libDeviceDiscovery.a *.o
endif
	@echo "libqcdev.so Created"
	@rm -f *.o

memcheck: clean
	@echo "Making libqcdev (memcheck)"
ifneq (,$(findstring Red Hat Enterprise Linux,$(OS_RELEASE)))
	@g++ -c -g qcdev.cpp QdIoQmi.cpp qdaio.cpp $(COMMON_SOURCE) -fPIC -I $(INCLUDE_FLAGS) -I $(INCLUDE_COMMON) $(LIBUSB_INCLUDE) $(CFLAGS) -fpermissive -w -D QCDEV_LOAD_LIBRARY -fsanitize=leak
	@g++ -o libqcdev.so -shared *.o $(LIBUSB_REDHAT_LIBS) -g -lpthread -ldl -w
	@ar rcs libDeviceDiscovery.a *.o
else
	@g++ -c -g qcdev.cpp QdIoQmi.cpp qdaio.cpp $(LIBUSB_SOURCE) $(LIBUSB_LOADER) $(COMMON_SOURCE) -fPIC -I $(LIBUSB_HEADER) -I $(INCLUDE_FLAGS) -I $(INCLUDE_COMMON) $(LIBUSB_INCLUDE) $(CFLAGS) -fpermissive -w -D QCDEV_LOAD_LIBRARY -fsanitize=leak
	@g++ -o libqcdev.so -shared *.o $(LIBUDEV_LIBS) -g -lpthread -ldl -w
	@ar rcs libDeviceDiscovery.a *.o
endif
	@echo "libqcdev.so Created"
	@rm -f *.o

clean:
	@rm -f *.so *.o *.a
	@echo "Files Cleared"
