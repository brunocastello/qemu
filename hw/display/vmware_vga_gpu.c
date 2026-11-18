/*
 * vmware-svga: in-process display owner for VirtualIntelMac's
 * PassthroughOpenGL host executor.
 *
 * VirtualIntelMac's app owns the display of the executor: the executor
 * seals a finished frame as an IOSurface and sends it, as a Mach port, to
 * a receive port the app listens on (VIMDisplayConnection.m); the app shows
 * the surface and tells the executor with a send-once reply when it no
 * longer reads it. Here QEMU is the app. The receive port lives in this
 * process and the executor reaches it through VIM_OPENGL_DISPLAY_INPROCESS
 * (see vim-display-inprocess.patch), so no bootstrap service is involved.
 *
 * A receive thread (VIMSurfaceFrameReceive, vim-display-surface-message.h,
 * MIT, from VIM) enforces VIMDisplayConnection's generation/ticket order and
 * hands the newest frame to the main loop, where it is wrapped as an
 * MTLTexture and shown through dpy_gl_scanout_texture() + dpy_gl_update()
 * (SPICE: ui/spice-display-metal.m blits it into UTM's shared IOSurface).
 *
 * Frame lifetime: the sender recycles a surface once its release reply
 * arrives, so a frame is released only after nothing reads it. The listeners
 * read a scanout texture only in the draw issued by dpy_gl_update(), and
 * that draw holds the console's GL block until it completed. A new frame is
 * therefore presented only while the console is not blocked, and the frame it
 * supersedes is released when the console tells us its scanout is replaced
 * (the cleanup callback), by which time its draw is complete. Frames that
 * arrive while another still waits to be presented replace it and are
 * released as skipped.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "qemu/timer.h"
#include "vim-display-surface-message.h"
#include "vmware_vga_gpu.h"

typedef struct GpuFrame {
    struct VIMSurfaceFrameMessage msg;
    IOSurfaceRef surface;
    void *texture; /* retained MTLTexture once presented */
} GpuFrame;

struct VMSVGAGpuDisplay {
    QemuConsole *con;
    mach_port_t port; /* receive right */
    QemuThread thread;
    bool thread_started;
    bool stopping; /* atomic */

    QemuMutex lock; /* pending, shared with the receive thread */
    GpuFrame *pending; /* newest received, not yet presented */

    /* main loop (BQL) only */
    QEMUBH *bh;
    GpuFrame *current; /* the console's scanout texture */
    int64_t last_present_ms; /* QEMU_CLOCK_REALTIME, 0 before the first */
    bool first_frame_logged;
};

static VMSVGAGpuDisplay *gpu_display;
static GMutex gpu_display_lock; /* gpu_display and thread start */

static void gpu_frame_release(GpuFrame *f, uint32_t status)
{
    if (f->texture) {
        vmsvga_metal_texture_release(f->texture);
    }
    CFRelease(f->surface);
    VIMSurfaceFrameRelease(&f->msg, status, 0);
    g_free(f);
}

static void *gpu_receive_thread(void *opaque)
{
    VMSVGAGpuDisplay *g = opaque;
    uid_t user = geteuid();
    pid_t renderer = getpid(); /* the executor is loaded into this process */
    uint64_t generation = 0, ticket = 0;

    while (!qatomic_read(&g->stopping)) {
        struct VIMSurfaceFrameMessage msg;
        IOSurfaceRef surface;
        kern_return_t kr = VIMSurfaceFrameReceive(g->port, user, renderer,
                                                  &msg, &surface, 250);
        if (kr == MACH_RCV_TIMED_OUT) {
            continue; /* nothing yet */
        }
        if (kr == KERN_INVALID_ARGUMENT) {
            /* a bad message, already destroyed */
            warn_report_once("vmware-svga: rejected a GPU frame message "
                             "(format, sender or IOSurface lookup)");
            continue;
        }
        if (kr) {
            if (kr == MACH_RCV_TOO_LARGE) {
                error_report("vmware-svga: oversized message on the display "
                             "port; GPU display stopped");
            } else if (!qatomic_read(&g->stopping)) {
                error_report("vmware-svga: GPU display port receive failed "
                             "(0x%x); GPU display stopped", kr);
            }
            break; /* port destroyed, or unusable */
        }
        if (!generation) {
            info_report("vmware-svga: first GPU frame received (%ux%u)",
                        msg.width, msg.height);
        }
        if (msg.generation < generation ||
            (msg.generation == generation && msg.ticket <= ticket)) {
            /* stale or replayed (VIMDisplayConnection.m receiveSurface:) */
            CFRelease(surface);
            VIMSurfaceFrameRelease(&msg, VIMSurfaceSkipped, 0);
            continue;
        }
        generation = msg.generation;
        ticket = msg.ticket;

        GpuFrame *f = g_new0(GpuFrame, 1);
        f->msg = msg;
        f->surface = surface;
        qemu_mutex_lock(&g->lock);
        GpuFrame *old = g->pending;
        g->pending = f;
        qemu_mutex_unlock(&g->lock);
        if (old) {
            gpu_frame_release(old, VIMSurfaceSkipped);
        }
        qemu_bh_schedule(g->bh);
    }
    return NULL;
}

/* The console replaced or disabled the scanout that showed this texture. */
static void gpu_scanout_cleanup(ScanoutTextureNative *native)
{
    VMSVGAGpuDisplay *g = gpu_display;

    if (!g || !g->current || g->current->texture != native->handle) {
        return;
    }
    GpuFrame *f = g->current;
    g->current = NULL;
    gpu_frame_release(f, VIMSurfaceGPUCompleted);
}

static bool gpu_frame_valid(const GpuFrame *f)
{
    /* VIMMetalView -presentSurface:width:height:completion: */
    IOSurfaceRef s = f->surface;
    return IOSurfaceGetPixelFormat(s) == 0x42475241 /* 'BGRA' */ &&
           IOSurfaceGetPlaneCount(s) == 0 &&
           IOSurfaceGetBytesPerElement(s) == 4 &&
           IOSurfaceGetWidth(s) >= f->msg.width &&
           IOSurfaceGetHeight(s) >= f->msg.height &&
           IOSurfaceGetBytesPerRow(s) >= f->msg.width * 4;
}

static void gpu_present_bh(void *opaque)
{
    VMSVGAGpuDisplay *g = opaque;

    if (qemu_console_is_gl_blocked(g->con)) {
        return; /* a draw is in flight; gl_block(false) calls us again */
    }
    qemu_mutex_lock(&g->lock);
    GpuFrame *f = g->pending;
    g->pending = NULL;
    qemu_mutex_unlock(&g->lock);
    if (!f) {
        return;
    }
    if (!console_has_gl(g->con)) {
        warn_report_once("vmware-svga: GPU frame skipped: the display has no GL");
        gpu_frame_release(f, VIMSurfaceSkipped);
        return;
    }
    if (!gpu_frame_valid(f)) {
        warn_report_once("vmware-svga: GPU frame skipped: IOSurface %zux%zu "
                         "format 0x%x for a %ux%u frame",
                         IOSurfaceGetWidth(f->surface),
                         IOSurfaceGetHeight(f->surface),
                         (unsigned)IOSurfaceGetPixelFormat(f->surface),
                         f->msg.width, f->msg.height);
        gpu_frame_release(f, VIMSurfaceSkipped);
        return;
    }
    if (!(f->texture = vmsvga_metal_texture_create(f->surface))) {
        warn_report_once("vmware-svga: GPU frame skipped: no Metal texture "
                         "for its IOSurface");
        gpu_frame_release(f, VIMSurfaceSkipped);
        return;
    }

    uint32_t w = f->msg.width, h = f->msg.height;
    ScanoutTextureNative native = {
        .type = SCANOUT_TEXTURE_NATIVE_TYPE_METAL,
        .handle = f->texture,
    };
    /*
     * The surface's rows run top to bottom, like virtio-gpu's Metal blob
     * scanout, which also passes backing_y_0_top=false. This replaces the
     * console's previous scanout, whose cleanup releases the previous frame.
     */
    dpy_gl_scanout_texture(g->con, 0, false, w, h, 0, 0, w, h,
                           native, gpu_scanout_cleanup);
    g->current = f;
    if (!g->first_frame_logged) {
        g->first_frame_logged = true;
        info_report("vmware-svga: first GPU frame presented (%ux%u)", w, h);
    }
    dpy_gl_update(g->con, 0, 0, w, h);
    g->last_present_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
}

VMSVGAGpuDisplay *vmsvga_gpu_display_new(QemuConsole *con)
{
    VMSVGAGpuDisplay *g;

    g_mutex_lock(&gpu_display_lock);
    if (gpu_display) {
        g_mutex_unlock(&gpu_display_lock);
        warn_report("vmware-svga: the in-process display already has an owner");
        return NULL;
    }
    g = g_new0(VMSVGAGpuDisplay, 1);
    g->con = con;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
                           &g->port)) {
        g_mutex_unlock(&gpu_display_lock);
        g_free(g);
        warn_report("vmware-svga: cannot allocate the display port");
        return NULL;
    }
    qemu_mutex_init(&g->lock);
    g->bh = qemu_bh_new(gpu_present_bh, g);
    gpu_display = g;
    g_mutex_unlock(&gpu_display_lock);
    return g;
}

void vmsvga_gpu_display_free(VMSVGAGpuDisplay *g)
{
    if (!g) {
        return;
    }
    g_mutex_lock(&gpu_display_lock);
    if (gpu_display == g) {
        gpu_display = NULL;
    }
    qatomic_set(&g->stopping, true);
    /* Destroying the receive right wakes the thread with PORT_DIED. */
    mach_port_destroy(mach_task_self(), g->port);
    if (g->thread_started) {
        qemu_thread_join(&g->thread);
    }
    g_mutex_unlock(&gpu_display_lock);

    qemu_bh_delete(g->bh);
    if (g->pending) {
        gpu_frame_release(g->pending, VIMSurfaceSkipped);
    }
    if (g->current) {
        /* The console must not keep a texture we are about to release;
         * the cleanup callback releases the frame (it needs gpu_display). */
        GpuFrame *f = g->current;
        g->current = NULL;
        dpy_gl_scanout_disable(g->con);
        gpu_frame_release(f, VIMSurfaceGPUCompleted);
    }
    qemu_mutex_destroy(&g->lock);
    g_free(g);
}

bool vmsvga_gpu_display_active(VMSVGAGpuDisplay *g)
{
    return g && g->current;
}

int64_t vmsvga_gpu_display_idle_ms(VMSVGAGpuDisplay *g)
{
    if (!g || !g->last_present_ms) {
        return INT64_MAX;
    }
    return qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - g->last_present_ms;
}

void vmsvga_gpu_display_gl_block(VMSVGAGpuDisplay *g, bool block)
{
    if (g && !block) {
        qemu_bh_schedule(g->bh);
    }
}

mach_port_t vmsvga_gpu_display_port(void)
{
    mach_port_t port = MACH_PORT_NULL;

    g_mutex_lock(&gpu_display_lock);
    VMSVGAGpuDisplay *g = gpu_display;
    /* IOSurfaces are only worth sending when a listener can show them. */
    if (g && !qatomic_read(&g->stopping) && console_has_gl(g->con)) {
        if (!g->thread_started) {
            qemu_thread_create(&g->thread, "vmsvga-display",
                               gpu_receive_thread, g, QEMU_THREAD_JOINABLE);
            g->thread_started = true;
            info_report("vmware-svga: in-process GPU display owner active");
        }
        if (!mach_port_insert_right(mach_task_self(), g->port, g->port,
                                    MACH_MSG_TYPE_MAKE_SEND)) {
            port = g->port;
        }
    }
    g_mutex_unlock(&gpu_display_lock);
    return port;
}
