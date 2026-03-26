#ifndef __SCRATCHPAD_MEM_SPM_DMA_ENGINE_HH__
#define __SCRATCHPAD_MEM_SPM_DMA_ENGINE_HH__

#include <cstdint>
#include <deque>
#include <unordered_map>

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
 * Supports two programming interfaces:
 *   1) MMIO: write SRC, DST, then LEN (writing LEN enqueues a transfer)
 *   2) Custom ISA: spm.dma enqueues, spm.dma.w waits for all completion
 *
 * The descriptor queue allows multiple transfers to be queued (default 4),
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
     * Enqueue a DMA transfer.  Returns true if successfully queued,
     * false if the descriptor queue is full (caller should retry).
     */
    bool startCopy(Addr src, Addr dst, uint64_t len);

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

    /**
     * Look up the DMA engine registered for the given System.
     * Returns nullptr if none is registered.
     */
    static SpmDmaEngine *lookup(System *sys);

  private:

    struct Descriptor {
        Addr src;
        Addr dst;
        uint64_t len;
    };

    static constexpr Addr REG_SRC    = 0x00;
    static constexpr Addr REG_DST    = 0x08;
    static constexpr Addr REG_LEN    = 0x10;
    static constexpr Addr REG_STATUS = 0x18;

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

    // ---- Descriptor queue ----
    std::deque<Descriptor> descQueue;
    unsigned maxDescriptors;

    // ---- Current transfer state ----
    State state;
    Addr curSrc;
    Addr curDst;
    uint64_t curLen;
    uint8_t *buffer;

    // Staged MMIO register values (written via SRC/DST before LEN enqueues)
    Addr stagedSrc;
    Addr stagedDst;

    /** Tick at which the current transfer was initiated. */
    Tick transferStartTick;

    PacketPtr pendingStatusPkt;

    EventFunctionWrapper beginReadEvent;
    EventFunctionWrapper readDoneEvent;
    EventFunctionWrapper writeDoneEvent;

    // Per-System registry (replaces singleton)
    static std::unordered_map<System*, SpmDmaEngine*> registry;

    struct DmaStats : public statistics::Group
    {
        DmaStats(SpmDmaEngine &engine);
        void regStats() override;

        statistics::Scalar transfers;
        statistics::Scalar bytesTransferred;
        statistics::Scalar busyTicks;
        statistics::Formula avgLatency;
        statistics::Scalar queueFullStalls;
    } dmaStats;

    Tick handleRead(PacketPtr pkt);
    Tick handleWrite(PacketPtr pkt);

    void startNextTransfer();
    void beginRead();
    void readDone();
    void writeDone();
    void transferComplete();
};

} // namespace gem5

#endif // __SCRATCHPAD_MEM_SPM_DMA_ENGINE_HH__
