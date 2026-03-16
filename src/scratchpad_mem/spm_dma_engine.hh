#ifndef __SCRATCHPAD_MEM_SPM_DMA_ENGINE_HH__
#define __SCRATCHPAD_MEM_SPM_DMA_ENGINE_HH__

#include <cstdint>

#include "dev/dma_device.hh"
#include "mem/tport.hh"
#include "params/SpmDmaEngine.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"
#include "sim/system.hh"

namespace gem5
{

class SpmDmaEngine : public ClockedObject
{
  public:
    PARAMS(SpmDmaEngine);
    SpmDmaEngine(const Params &p);
    ~SpmDmaEngine();

    Port &getPort(const std::string &if_name, PortID idx) override;
    void init() override;

    void startCopy(Addr src, Addr dst, uint64_t len);
    bool isComplete() const { return state == Idle; }

    static SpmDmaEngine *getInstance() { return instance; }

  private:
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
    Tick initLatency;

    State state;
    Addr srcAddr;
    Addr dstAddr;
    uint64_t totalLen;
    uint8_t *buffer;

    PacketPtr pendingStatusPkt;

    EventFunctionWrapper beginReadEvent;
    EventFunctionWrapper readDoneEvent;
    EventFunctionWrapper writeDoneEvent;

    static SpmDmaEngine *instance;

    Tick handleRead(PacketPtr pkt);
    Tick handleWrite(PacketPtr pkt);

    void beginRead();
    void readDone();
    void writeDone();
    void transferComplete();
};

} // namespace gem5

#endif // __SCRATCHPAD_MEM_SPM_DMA_ENGINE_HH__
