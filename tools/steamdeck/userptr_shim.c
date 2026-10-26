/* userptr_shim: LD_PRELOAD library for the HC09 PC recompile on AMD Linux GPUs (Steam Deck), from a Steam Deck
 * owner's guide (2026-10-24; tested on a Deck LCD, SteamOS 3.9.5, release 1.1.1).
 *
 * The game's renderer imports its 512 MiB guest-memory section into Vulkan (VK_EXT_external_memory_host). Under
 * Wine/Proton that section is a SHARED, file-backed mapping. libdrm / RADV register host memory with the amdgpu
 * kernel driver through DRM_IOCTL_AMDGPU_GEM_USERPTR with AMDGPU_GEM_USERPTR_ANONONLY set, and the kernel refuses
 * file-backed pages under that flag (-EPERM), so vkAllocateMemory(import) fails and the game shows a black screen.
 * This shim clears ANONONLY on that one ioctl and passes everything else through unchanged.
 *
 * Built without libc headers so it cross-compiles anywhere (tools/steamdeck/build.cmd on Windows):
 *   clang --target=x86_64-unknown-linux-gnu -O2 -fPIC -shared -nostdlib -fuse-ld=lld userptr_shim.c -o libhc09_userptr.so
 * Used through LD_PRELOAD, APPENDED to Steam's own (it carries the overlay); the launcher does that by itself under
 * Wine/Proton when libhc09_userptr.so is beside it. HC09_USERPTR_LOG=1 prints one line per rewritten call to stderr.
 */
typedef unsigned long u64;
typedef unsigned int u32;

extern void* dlsym(void* handle, const char* name);
extern char* getenv(const char* name);
extern int dprintf(int fd, const char* fmt, ...);
#define RTLD_NEXT ((void*)-1l)

/* DRM_IOWR(DRM_COMMAND_BASE + DRM_AMDGPU_GEM_USERPTR, struct drm_amdgpu_gem_userptr):
 * _IOWR('d', 0x40 + 0x11, 24) = (3 << 30) | (24 << 16) | ('d' << 8) | 0x51 */
#define DRM_IOCTL_AMDGPU_GEM_USERPTR 0xC0186451ul
#define AMDGPU_GEM_USERPTR_ANONONLY 0x2u

struct drm_amdgpu_gem_userptr {
    u64 addr;
    u64 size;
    u32 flags;
    u32 handle;
};

typedef int (*ioctl_fn)(int, unsigned long, void*);

int ioctl(int fd, unsigned long request, void* arg)
{
    static ioctl_fn real;
    if (!real) real = (ioctl_fn)dlsym(RTLD_NEXT, "ioctl");
    if ((request & 0xFFFFFFFFul) == DRM_IOCTL_AMDGPU_GEM_USERPTR && arg) {
        struct drm_amdgpu_gem_userptr* u = (struct drm_amdgpu_gem_userptr*)arg;
        if (u->flags & AMDGPU_GEM_USERPTR_ANONONLY) {
            u->flags &= ~AMDGPU_GEM_USERPTR_ANONONLY;
            if (getenv("HC09_USERPTR_LOG"))
                dprintf(2, "[userptr_shim] cleared ANONONLY: addr %lx size %lx flags %x\n", u->addr, u->size, u->flags);
        }
    }
    return real(fd, request, arg);
}
