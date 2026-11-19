# SPDX-License-Identifier: GPL-2.0
#
# OpenVFD - driver for the small front panel LED/segment display of Android
# TV boxes (FD628/FD650 and friends).  Vendored from
# https://github.com/arthur-liberman/linux_openvfd (GPL-2.0), unmodified
# sources; only this in-tree Makefile replaces the standalone one.
#
obj-$(CONFIG_OPENVFD) += openvfd.o

openvfd-objs := protocols/i2c_sw.o
openvfd-objs += protocols/i2c_hw.o
openvfd-objs += protocols/spi_sw.o
openvfd-objs += controllers/dummy.o
openvfd-objs += controllers/seg7_ctrl.o
openvfd-objs += controllers/fd628.o
openvfd-objs += controllers/fd650.o
openvfd-objs += controllers/hd44780.o
openvfd-objs += controllers/gfx_mono_ctrl.o
openvfd-objs += controllers/ssd1306.o
openvfd-objs += controllers/pcd8544.o
openvfd-objs += controllers/il3829.o
openvfd-objs += openvfd_drv.o
