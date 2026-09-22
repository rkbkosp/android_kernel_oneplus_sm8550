# SPDX-License-Identifier: GPL-2.0-only
# Oplus RAM and background-freeze built-ins. Object names that must
# show up as /sys/module/<name>/ are the translation unit names below.

ccflags-y += -I$(srctree)/drivers/android

obj-$(CONFIG_OPLUS_MM_BG) += sched_assist.o
obj-$(CONFIG_OPLUS_MM_BG) += schedinfo.o
obj-$(CONFIG_OPLUS_MM_BG) += oplus_sys_hans.o
obj-$(CONFIG_KUNIT) += policy_kunit.o
