.. SPDX-License-Identifier: GPL-2.0-only

=====================================
 accel/rocket Rockchip NPU driver
=====================================

The accel/rocket driver supports the Neural Processing Units (NPUs) inside some Rockchip SoCs such as the RK3588. Rockchip calls it RKNN and sometimes RKNPU.

This NPU is closely based on the NVDLA IP released by NVIDIA as open hardware in 2018, along with open source kernel and userspace drivers.

The frontend unit in Rockchip's NPU though is completely different from that in the open source IP, so this kernel driver is specific to Rockchip's version.

The hardware is described in chapter 36 in the RK3588 TRM.

This driver just powers the hardware on and off, allocates and maps buffers to the device and submits jobs to the frontend unit. Everything else is done in userspace, as a Gallium driver that is part of the Mesa3D project.

Hardware currently supported:

* RK3588