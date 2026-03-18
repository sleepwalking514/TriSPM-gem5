#ifndef __SCRATCHPAD_MEM_SCRATCHPAD_MEMORY_HH__
#define __SCRATCHPAD_MEM_SCRATCHPAD_MEMORY_HH__

#include <list>
#include <vector>

#include "base/random.hh"
#include "mem/abstract_mem.hh"
#include "mem/port.hh"
#include "params/ScratchpadMemory.hh"

namespace gem5
{

namespace memory
{

/**
 * Multi-banked scratchpad memory with per-bank conflict modeling.
 *
 * Address-to-bank mapping uses word-level interleaving:
 *   bank = ((addr - base) / interleave_size) % num_banks
 *
 * When two accesses hit the same bank within its latency window,
 * the later one observes additional latency (bank conflict).
 * Accesses to different banks proceed without penalty.
 */
class ScratchpadMemory : public AbstractMemory
{

  private:

    class DeferredPacket
    {
      public:
        const Tick tick;
        const PacketPtr pkt;
        DeferredPacket(PacketPtr _pkt, Tick _tick) : tick(_tick), pkt(_pkt) {}
    };

    class MemoryPort : public ResponsePort
    {
      private:
        ScratchpadMemory& mem;

      public:
        MemoryPort(const std::string& _name, ScratchpadMemory& _memory);

      protected:
        Tick recvAtomic(PacketPtr pkt) override;
        Tick recvAtomicBackdoor(
                PacketPtr pkt, MemBackdoorPtr &_backdoor) override;
        void recvFunctional(PacketPtr pkt) override;
        void recvMemBackdoorReq(const MemBackdoorReq &req,
                MemBackdoorPtr &backdoor) override;
        bool recvTimingReq(PacketPtr pkt) override;
        void recvRespRetry() override;
        AddrRangeList getAddrRanges() const override;
    };

    MemoryPort port;

    const Tick latency;
    const Tick latencyVar;
    const double bandwidth;

    const unsigned numBanks;
    const unsigned bankIntlvSize;

    /** Tick at which each bank becomes free. */
    std::vector<Tick> bankBusyUntil;

    std::list<DeferredPacket> packetQueue;

    bool isBusy;
    bool retryReq;
    bool retryResp;

    mutable Random::RandomPtr rng = Random::genRandom();

    void release();
    EventFunctionWrapper releaseEvent;

    void dequeue();
    EventFunctionWrapper dequeueEvent;

    Tick getLatency() const;

    /** Map a byte address to its bank index. */
    unsigned addrToBank(Addr addr) const;

    std::unique_ptr<Packet> pendingDelete;

    struct SpmStats : public statistics::Group
    {
        SpmStats(ScratchpadMemory &spm);
        void regStats() override;

        const ScratchpadMemory &spm;

        statistics::Scalar reads;
        statistics::Scalar writes;

        statistics::Scalar readLatency;
        statistics::Scalar writeLatency;
        statistics::Formula avgReadLatency;
        statistics::Formula avgWriteLatency;

        statistics::Scalar bankConflicts;

        statistics::Vector perBankReads;
        statistics::Vector perBankWrites;
        statistics::Vector perBankConflicts;
    } spmStats;

  public:

    ScratchpadMemory(const ScratchpadMemoryParams &p);

    DrainState drain() override;
    Port &getPort(const std::string &if_name,
                  PortID idx=InvalidPortID) override;
    void init() override;

  protected:
    Tick recvAtomic(PacketPtr pkt);
    Tick recvAtomicBackdoor(PacketPtr pkt, MemBackdoorPtr &_backdoor);
    void recvFunctional(PacketPtr pkt);
    void recvMemBackdoorReq(const MemBackdoorReq &req,
            MemBackdoorPtr &backdoor);
    bool recvTimingReq(PacketPtr pkt);
    void recvRespRetry();
};

} // namespace memory
} // namespace gem5

#endif // __SCRATCHPAD_MEM_SCRATCHPAD_MEMORY_HH__
