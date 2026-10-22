/* Link every late memory-header API against its production C definition. */
#include "xbox_memory_layout.h"
#include <cstdlib>

extern "C" void *recomp_lookup(ULONG) { std::abort(); }
extern "C" void *recomp_lookup_manual(ULONG) { std::abort(); }

int main()
{
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
    return !(configure && stack && tib && release && reset && acquire && recycle &&
             set_thread && get_thread && mode);
}
