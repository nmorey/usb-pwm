# Makefile for building the usb_pwm_hwmon kernel module.

obj-m += usb_pwm_hwmon.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

dist:
	rm -rf usb-pwm-hwmon-1.0
	mkdir -p usb-pwm-hwmon-1.0
	cp usb_pwm_hwmon.c Makefile usb-pwm-hwmon.spec usb-pwm-hwmon-1.0/
	tar -czf usb-pwm-hwmon-1.0.tar.gz usb-pwm-hwmon-1.0/
	rm -rf usb-pwm-hwmon-1.0

rpm: dist
	rpmbuild -ta usb-pwm-hwmon-1.0.tar.gz

