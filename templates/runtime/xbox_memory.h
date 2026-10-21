/* Memory layout is provided by the runtime, parsed from the loaded XBE.
 * Do not copy section addresses or maintain a second allocator in a port.
 * Link xboxrecomp (or xbox_kernel) and use xbox_MemoryLayoutInit,
 * xbox_MemoryLayoutShutdown, xbox_GetMemoryBase and xbox_GetMemoryOffset.
 * See src/kernel/xbox_memory_layout.h for the supported public interface. */
#ifndef XBOX_MEMORY_H
#define XBOX_MEMORY_H
#include "xbox_memory_layout.h"
#endif
