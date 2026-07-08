#ifndef __SCRATCHPAD_MEM_SPM_DMA_ENGINE_HH__
#define __SCRATCHPAD_MEM_SPM_DMA_ENGINE_HH__

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include "dev/dma_device.hh"
#include "mem/tport.hh"
#include "params/SpmDmaEngine.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"
#include "sim/stats.hh"
#include "sim/system.hh"

namespace gem5
{

/**
 * DMA engine for scratchpad memory with a multi-entry descriptor queue.
 *
 * Supports 1D (linear) and 2D (strided) transfers.
 *
 * Programming interfaces:
 *   1) MMIO: write SRC, DST, [SRC_STRIDE, DST_STRIDE, HEIGHT,] then LEN
 *      (writing LEN enqueues a transfer).  When HEIGHT <= 1 or stride
 *      registers are never written, the engine operates in 1D mode.
 *   2) Custom ISA: spm.dma enqueues (1D), spm.dma.w waits for completion
 *
 * 2D transfers copy HEIGHT rows of LEN bytes each, advancing the source
 * pointer by SRC_STRIDE and the destination pointer by DST_STRIDE per
 * row.  Internally the engine issues all row reads in parallel and
 * starts writing each row as soon as its read completes, fully
 * pipelining read/write across rows for maximum throughput.
 *
 * The descriptor queue allows multiple transfers to be queued (default 32),
 * enabling the compiler to overlap DMA load/store with computation
 * (e.g., double-buffered tiling without pipeline stalls between submit).
 *
 * Per-System registry replaces the previous singleton pattern, making
 * the design extensible to multi-core configurations.
 */
class SpmDmaEngine : public ClockedObject
{
  public:
    PARAMS(SpmDmaEngine);
    SpmDmaEngine(const Params &p);
    ~SpmDmaEngine();

    Port &getPort(const std::string &if_name, PortID idx) override;
    void init() override;
    DrainState drain() override;

    /**
     * Enqueue a DMA transfer.  The current software interfaces do not
     * implement enqueue retry, so a full descriptor queue is a fatal
     * configuration/programming error instead of a dropped transfer.
     *
     * 1D form: startCopy(src, dst, len) — len contiguous bytes.
     * 2D form: startCopy(src, dst, width, srcStride, dstStride, height)
     *          — copies height rows of width bytes each.
     */
    bool startCopy(Addr src, Addr dst, uint64_t len,
                   uint64_t srcStride = 0, uint64_t dstStride = 0,
                   uint32_t height = 1);

    /** Stage stride values for a subsequent startCopy (ISA path). */
    void setStride(uint64_t srcStride, uint64_t dstStride) {
        stagedSrcStride = srcStride;
        stagedDstStride = dstStride;
    }

    /** Read back staged strides (for ISA 2D instruction). */
    uint64_t getStagedSrcStride() const { return stagedSrcStride; }
    uint64_t getStagedDstStride() const { return stagedDstStride; }

    /** True when all transfers have completed and the queue is empty. */
    bool isComplete() const { return state == Idle && descQueue.empty(); }

    /** True when the descriptor queue has no free entries. */
    bool queueFull() const {
        return descQueue.size() >= maxDescriptors;
    }

    /** Number of in-flight + queued transfers (0 = idle). */
    unsigned pendingCount() const {
        return descQueue.size() + (state != Idle ? 1 : 0);
    }

    /** Record retired XSPM custom DMA instructions. */
    void recordXspmDma1D();
    void recordXspmDmaWait();
    void recordXspmDmaStride();
    void recordXspmDma2D();

    /**
     * Look up the DMA engine registered for the given System.
     * Returns nullptr if none is registered.
     */
    static SpmDmaEngine *lookup(System *sys);

  private:

    struct Descriptor {
        Addr src;
        Addr dst;
        uint64_t len;       // 1D: total bytes; 2D: bytes per row (width)
        uint64_t srcStride; // 2D: source row pitch in bytes (0 = 1D)
        uint64_t dstStride; // 2D: dest row pitch in bytes (0 = 1D)
        uint32_t height;    // 2D: number of rows (0 or 1 = 1D mode)
    };

    static constexpr Addr REG_SRC            = 0x00;
    static constexpr Addr REG_DST            = 0x08;
    // REG_LEN: writing triggers enqueue.  Lower 32 bits are LEN (bytes per
    // row in 2D mode, total bytes in 1D mode).  Upper 32 bits, when non-
    // zero, override the staged HEIGHT in one store — letting the
    // compiler skip a separate REG_HEIGHT write per descriptor.
    static constexpr Addr REG_LEN            = 0x10;
    static constexpr Addr REG_STATUS         = 0x18;
    static constexpr Addr REG_SRC_STRIDE     = 0x20;
    static constexpr Addr REG_DST_STRIDE     = 0x28;
    static constexpr Addr REG_HEIGHT         = 0x30;
    // REG_STRIDES_PACKED: lower 32 = SRC_STRIDE, upper 32 = DST_STRIDE.
    // Lets the compiler set both row pitches in one MMIO store, halving
    // the descriptor traffic for 2D enqueues.  Legacy REG_SRC_STRIDE /
    // REG_DST_STRIDE remain functional for hand-written code.
    static constexpr Addr REG_STRIDES_PACKED = 0x38;

    enum State { Idle, Reading, Writing };

    class PioPort : public SimpleTimingPort
    {
        SpmDmaEngine &engine;

      public:
        PioPort(const std::string &name, SpmDmaEngine *owner);

        Tick recvAtomic(PacketPtr pkt) override;
        bool recvTimingReq(PacketPtr pkt) override;
        void recvFunctional(PacketPtr pkt) override;
        AddrRangeList getAddrRanges() const override;
    };

    System *system;
    PioPort pioPort;
    DmaPort dmaPort;

    Addr pioAddr;
    Addr pioSize;
    Tick pioDelay;
    Tick descLatency;
    Addr spmAddr;
    Addr spmSize;

    // ---- Descriptor queue ----
    std::deque<Descriptor> descQueue;
    unsigned maxDescriptors;

    // ---- Current transfer state ----
    State state;
    Addr curSrc;
    Addr curDst;
    uint64_t curLen;

    // 1D transfer buffer (used only when curHeight <= 1)
    uint8_t *buffer;

    // 2D pipelined state
    uint32_t curHeight;    // total rows (1 for 1D transfers)
    uint64_t curSrcStride; // source row pitch in bytes
    uint64_t curDstStride; // destination row pitch in bytes

    // For pipelined 2D: per-row buffers and event objects
    std::vector<uint8_t*> rowBuffers;
    std::vector<EventFunctionWrapper*> rowReadDoneEvents;
    std::vector<EventFunctionWrapper*> rowWriteDoneEvents;
    int rowsWriteComplete;  // counter for completed row writes

    // Staged MMIO register values (written via SRC/DST before LEN enqueues)
    Addr stagedSrc;
    Addr stagedDst;
    uint64_t stagedSrcStride;
    uint64_t stagedDstStride;
    uint32_t stagedHeight;

    /** Tick at which the current transfer was initiated. */
    Tick transferStartTick;

    /**
     * Tick at which the first busy-poll arrived for the current
     * wait sequence (0 when no wait sequence is active).  Used to
     * measure total stall cycles from first poll to completion.
     */
    Tick waitStartTick;

    // Events for 1D path
    EventFunctionWrapper beginReadEvent;
    EventFunctionWrapper readDoneEvent;
    EventFunctionWrapper writeDoneEvent;

    // Event for 2D pipelined path: kicks off all row reads after desc_latency
    EventFunctionWrapper beginPipelinedReadEvent;

    // Per-System registry (replaces singleton)
    static std::unordered_map<System*, SpmDmaEngine*> registry;

    struct DmaStats : public statistics::Group
    {
        DmaStats(SpmDmaEngine &engine);
        void regStats() override;

        statistics::Scalar transfers;
        statistics::Scalar transfers2D;
        statistics::Scalar rowsTransferred;
        statistics::Scalar bytesTransferred;
        statistics::Scalar busyCycles;
        statistics::Formula avgLatency;
        statistics::Scalar queueFullStalls;

        /** Number of STATUS polls that found DMA still busy. */
        statistics::Scalar waitPollBusy;
        /** Number of STATUS polls that found DMA idle (completion). */
        statistics::Scalar waitPollIdle;
        /**
         * Accumulated stall cycles: elapsed cycles between the first
         * busy-poll and the subsequent idle-poll for each wait sequence.
         */
        statistics::Scalar waitStallCycles;
        /** Average stall cycles per wait sequence. */
        statistics::Formula avgWaitStallCycles;

        /** XSPM custom instruction execution counters. */
        statistics::Scalar xspmInsts;
        statistics::Scalar xspmDma1DInsts;
        statistics::Scalar xspmDmaWaitInsts;
        statistics::Scalar xspmDmaStrideInsts;
        statistics::Scalar xspmDma2DInsts;
    } dmaStats;

    Tick handleRead(PacketPtr pkt);
    Tick handleWrite(PacketPtr pkt);

    void startNextTransfer();
    void beginRead();        // 1D path: single read
    void readDone();         // 1D path: single read done
    void writeDone();        // 1D path: single write done
    void beginPipelinedRead(); // 2D path: issue all row reads
    void pipelinedRowReadDone(int row);  // 2D path: one row read done
    void pipelinedRowWriteDone(int row); // 2D path: one row write done
    void cleanupPipelinedState();        // 2D path: free per-row resources
    void transferComplete();

    // SE-mode VA→PA translation via the process page table.
    Addr translateAddr(Addr vaddr);

    bool rangeInSpm(Addr vaddr, uint64_t len) const;
    bool rangeOverlapsSpm(Addr vaddr, uint64_t len) const;

    // Issue a DMA read/write that may span multiple virtual pages.
    // The engine splits the request at VA page boundaries (4 KiB) and
    // translates each chunk separately, because consecutive VAs do not
    // necessarily map to consecutive PAs.  All sub-actions complete the
    // same `doneEvent` after the last one finishes.
    void issuePagedDmaAction(Packet::Command cmd, Addr vaddr, uint64_t len,
                             EventFunctionWrapper *doneEvent, uint8_t *buf,
                             Request::Flags flags = 0);

    // DMA writes to cacheable DRAM must invalidate possible CPU cache copies
    // before the data write, otherwise stale or dirty lines can survive above
    // the DMA agent in the classic cache hierarchy.
    void issueCoherentWrite(Addr vaddr, uint64_t len,
                            EventFunctionWrapper *doneEvent, uint8_t *buf);
};

} // namespace gem5

#endif // __SCRATCHPAD_MEM_SPM_DMA_ENGINE_HH__
