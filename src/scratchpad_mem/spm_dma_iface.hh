#ifndef __SCRATCHPAD_MEM_SPM_DMA_IFACE_HH__
#define __SCRATCHPAD_MEM_SPM_DMA_IFACE_HH__

#include "base/types.hh"

namespace gem5
{

class ThreadContext;

/**
 * ISA instruction interface for the SPM DMA engine.
 *
 * These free functions are called from Xspm custom instruction
 * implementations.  They look up the DMA engine registered for the
 * System that owns the calling ThreadContext.
 */

/** Enqueue a 1D DMA transfer.  Returns false if the descriptor queue is full. */
bool spmDmaStartCopy(ThreadContext *tc, Addr src, Addr dst, uint64_t len);

/** Enqueue a 2D strided DMA transfer.  Returns false if queue is full.
 *  Copies height rows of width bytes, advancing src by srcStride and
 *  dst by dstStride per row. */
bool spmDmaStartCopy2D(ThreadContext *tc, Addr src, Addr dst,
                       uint64_t width, uint32_t height,
                       uint64_t srcStride, uint64_t dstStride);

/** Stage stride values for the next 2D transfer (ISA instruction path). */
void spmDmaSetStride(ThreadContext *tc, uint64_t srcStride, uint64_t dstStride);

/** Enqueue a 2D transfer using strides staged by spmDmaSetStride.
 *  Reads srcStride/dstStride from the engine's staged state. */
bool spmDmaStartCopy2DStaged(ThreadContext *tc, Addr src, Addr dst,
                             uint64_t width, uint32_t height);

/** True when all queued and in-flight transfers have completed. */
bool spmDmaIsComplete(ThreadContext *tc);

/** True when the descriptor queue cannot accept more entries. */
bool spmDmaQueueFull(ThreadContext *tc);

constexpr uint64_t SPM_DMA_STATUS_ADDR = 0xF0000018ULL;

} // namespace gem5

#endif // __SCRATCHPAD_MEM_SPM_DMA_IFACE_HH__
