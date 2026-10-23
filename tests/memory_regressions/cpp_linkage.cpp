/* Link every late memory-header API against its production C definition. */
#include "xbox_memory_layout.h"
#include <cstdlib>

extern "C" void *recomp_lookup(ULONG) { std::abort(); }
extern "C" void *recomp_lookup_manual(ULONG) { std::abort(); }

int main()
{
    auto volatile kernel_alloc = &xbox_AllocKernelStack;
    auto volatile kernel_free = &xbox_FreeKernelStack;
    auto volatile bounds = &xbox_GetStackBounds;
    auto volatile worker_config = &xbox_ConfigureThreadStack;
    auto volatile sized_stack = &xbox_AllocThreadStackSized;
    auto volatile kernel_data = &xbox_ConfigureKernelData;
    auto volatile configure = &xbox_ConfigureMainStack;
    auto volatile stack = &xbox_AllocThreadStack;
    auto volatile tib = &xbox_AllocThreadTib;
    auto volatile release = &xbox_FreeThreadStack;
    auto volatile reset = &xbox_worker_stack_reset;
    auto volatile acquire = &xbox_worker_stack_alloc;
    auto volatile recycle = &xbox_worker_stack_free;
    auto volatile set_thread = &xbox_set_game_thread;
    auto volatile get_thread = &xbox_thread_debug_handle;
    auto volatile mode = &xbox_SetThreadMode;
    return !(kernel_alloc && kernel_free && bounds && worker_config && sized_stack && kernel_data && configure && stack && tib && release && reset && acquire && recycle &&
             set_thread && get_thread && mode);
}
