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
 * Multi-banked scratchpad memory with two response ports:
 *   port     — DMA / interconnect path (L2XBar)
 *   cpu_port — CPU direct path (tightly-coupled, low latency)
 *
 * Both ports share the same backing store and bank-conflict model.
 */
class ScratchpadMemory : public AbstractMemory
{

  private:

    static constexpr int PORT_BUS = 0;
    static constexpr int PORT_CPU = 1;
    static constexpr int NUM_PORTS = 2;

    class MemoryPort : public ResponsePort
    {
      private:
        ScratchpadMemory& mem;
        int id_;

      public:
        MemoryPort(const std::string& _name, ScratchpadMemory& _memory,
                   int id);
        int id() const { return id_; }

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

    class DeferredPacket
    {
      public:
        const Tick tick;
        const PacketPtr pkt;
        const int portId;
        DeferredPacket(PacketPtr _pkt, Tick _tick, int _portId)
            : tick(_tick), pkt(_pkt), portId(_portId) {}
    };

    MemoryPort port;
    MemoryPort cpuPort;

    MemoryPort& portById(int id) {
        return (id == PORT_CPU) ? cpuPort : port;
    }

    const Tick latency;
    const Tick latencyVar;
    const double bandwidth;

    const unsigned numBanks;
    const unsigned bankIntlvSize;

    std::vector<Tick> bankBusyUntil;

    std::list<DeferredPacket> packetQueue;

    bool isBusy;
    bool retryReq_[NUM_PORTS];
    bool retryResp_[NUM_PORTS];

    mutable Random::RandomPtr rng = Random::genRandom();

    void release();
    EventFunctionWrapper releaseEvent;

    void dequeue();
    EventFunctionWrapper dequeueEvent;

    Tick getLatency() const;
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
    bool recvTimingReq(PacketPtr pkt, int portId);
    void recvRespRetry(int portId);
};

} // namespace memory
} // namespace gem5

#endif // __SCRATCHPAD_MEM_SCRATCHPAD_MEMORY_HH__
