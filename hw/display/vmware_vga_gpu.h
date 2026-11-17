/*
 * vmware-svga: in-process display owner for VirtualIntelMac's
 * PassthroughOpenGL host executor (frames as IOSurfaces, see
 * vmware_vga_gpu.c).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_DISPLAY_VMWARE_VGA_GPU_H
#define HW_DISPLAY_VMWARE_VGA_GPU_H

#include <mach/mach.h>
#include <IOSurface/IOSurface.h>
#include "ui/console.h"

typedef struct VMSVGAGpuDisplay VMSVGAGpuDisplay;

/* One owner per process (the executor supports one); NULL if taken. */
VMSVGAGpuDisplay *vmsvga_gpu_display_new(QemuConsole *con);
void vmsvga_gpu_display_free(VMSVGAGpuDisplay *g);
/* A GPU frame is the console's current scanout. */
bool vmsvga_gpu_display_active(VMSVGAGpuDisplay *g);
/* GraphicHwOps.gl_block: present a held frame once the listeners are idle. */
void vmsvga_gpu_display_gl_block(VMSVGAGpuDisplay *g, bool block);
/*
 * New send right to the owner's port, or MACH_PORT_NULL when the console
 * has no GL listener to show IOSurfaces. The executor calls this through
 * the address published in VIM_OPENGL_DISPLAY_INPROCESS.
 */
mach_port_t vmsvga_gpu_display_port(void);

/* vmware_vga_metal.m */
/* Retained MTLTexture (BGRA8) over the IOSurface, or NULL. */
void *vmsvga_metal_texture_create(IOSurfaceRef surface);
void vmsvga_metal_texture_release(void *texture);

#endif
