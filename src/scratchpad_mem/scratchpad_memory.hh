#ifndef __SCRATCHPAD_MEM_SCRATCHPAD_MEMORY_HH__
#define __SCRATCHPAD_MEM_SCRATCHPAD_MEMORY_HH__

#include "params/ScratchpadMemory.hh"
#include "mem/simple_mem.hh"

namespace gem5
{

namespace memory
{

class ScratchpadMemory: public SimpleMemory
{
    public:
        ScratchpadMemory(const ScratchpadMemoryParams& params);
};

}
}

#endif