include ./CONFIG

DEPENDS=
CROSSARCH=x64
PWD=$(shell pwd)
PACKAGE_DIR?=${PWD}

DOCKCROSS=$(PWD)/../dockcross
R2PLUGDIR=/usr/lib/radare2/$(VERSION)


all: root
	sudo rm -rf control data
	${MAKE} clean
	mkdir -p data
	cp -rf root/* data
	${MAKE} control
	${MAKE} deb

root:
	cd ../.. ; $(DOCKCROSS) --image dockcross/linux-$(CROSSARCH) bash -c make
	mkdir -p root/usr/sbin
	mkdir -p root/usr/share/man/man1
	cp -f ../../fsmod root/usr/sbin
	cp -f ../../fsmod.1 root/usr/share/man/man1/fsmod.1

purge: clean
	rm -rf root

summary:
	echo $(VERSION)

include deb.mk
