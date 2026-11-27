/*
 * vmware-svga: wrap a PassthroughOpenGL display IOSurface as an MTLTexture
 * for dpy_gl_scanout_texture (ScanoutTextureNative METAL). Same descriptor
 * as VirtualIntelMac's VIMMetalView -presentSurface:.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "vmware_vga_gpu.h"
#include <Metal/Metal.h>

void *vmsvga_metal_texture_create(IOSurfaceRef surface)
{
    static id<MTLDevice> device;

    if (!device) {
        device = MTLCreateSystemDefaultDevice();
        if (!device) {
            return NULL;
        }
    }
    @autoreleasepool {
        MTLTextureDescriptor *descriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               width:IOSurfaceGetWidth(surface)
                                                              height:IOSurfaceGetHeight(surface)
                                                           mipmapped:NO];
        descriptor.usage = MTLTextureUsageShaderRead;
        return [device newTextureWithDescriptor:descriptor iosurface:surface plane:0];
    }
}

void vmsvga_metal_texture_release(void *texture)
{
    [(id<MTLTexture>)texture release];
}
