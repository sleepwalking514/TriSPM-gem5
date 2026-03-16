#ifndef __SCRATCHPAD_MEM_SPM_DMA_IFACE_HH__
#define __SCRATCHPAD_MEM_SPM_DMA_IFACE_HH__

#include "base/types.hh"

namespace gem5
{

void spmDmaStartCopy(Addr src, Addr dst, uint64_t len);
bool spmDmaIsComplete();

constexpr uint64_t SPM_DMA_STATUS_ADDR = 0xF0000018ULL;

} // namespace gem5

#endif // __SCRATCHPAD_MEM_SPM_DMA_IFACE_HH__
