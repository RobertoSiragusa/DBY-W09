# Makefile for the smartpa devkit drivers.
#

# Each configuration option enables a list of files.
	ccflags-y += -DSMARTPAKIT_MISC_RW_PERMISSION_ENABLE
	ccflags-y += -I$(srctree)/drivers/devkit/audiokit/

ifeq ($(TARGET_BUILD_VARIANT),eng)
endif
ifdef CONFIG_HUAWEI_SMARTPAKIT_AUDIO
endif
obj-$(CONFIG_HUAWEI_SMARTPAKIT_AUDIO) += smartpakit.o

smartpakit_i2c-objs := smartpakit_i2c_ops.o
ifneq ($(TARGET_BUILD_VARIANT),user)
	smartpakit_i2c-objs += smartpakit_info.o
endif
obj-$(CONFIG_HUAWEI_SMARTPAKIT_AUDIO) += smartpakit_i2c.o
obj-$(CONFIG_SND_SOC_AU_PA) += external_requirement.o
obj-$(CONFIG_HUAWEI_ARMPC_PLATFORM) += smartpakit_i2c_pm.o
