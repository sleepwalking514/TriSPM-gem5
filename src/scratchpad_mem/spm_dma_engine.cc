#include "scratchpad_mem/spm_dma_engine.hh"

#include "base/trace.hh"
#include "debug/SpmDma.hh"
#include "mem/packet_access.hh"
#include "sim/system.hh"

namespace gem5
{

SpmDmaEngine *SpmDmaEngine::instance = nullptr;

// ---------------- PioPort ----------------

SpmDmaEngine::PioPort::PioPort(const std::string &name,
                                SpmDmaEngine *owner)
    : SimpleTimingPort(name, owner), engine(*owner)
{}

Tick
SpmDmaEngine::PioPort::recvAtomic(PacketPtr pkt)
{
    Tick delay;
    if (pkt->isRead())
        delay = engine.handleRead(pkt);
    else if (pkt->isWrite())
        delay = engine.handleWrite(pkt);
    else
        panic("SpmDmaEngine::PioPort: unexpected packet command\n");

    if (pkt->needsResponse())
        pkt->makeResponse();

    return delay;
}

bool
SpmDmaEngine::PioPort::recvTimingReq(PacketPtr pkt)
{
    Addr offset = pkt->getAddr() - engine.pioAddr;

    if (pkt->isRead() && offset == REG_STATUS && engine.state != Idle) {
        panic_if(engine.pendingStatusPkt,
                 "SpmDmaEngine: only one pending STATUS read supported");
        pkt->makeResponse();
        engine.pendingStatusPkt = pkt;
        return true;
    }

    return SimpleTimingPort::recvTimingReq(pkt);
}

void
SpmDmaEngine::PioPort::recvFunctional(PacketPtr pkt)
{
    if (pkt->isRead())
        engine.handleRead(pkt);
    else if (pkt->isWrite())
        engine.handleWrite(pkt);
}

AddrRangeList
SpmDmaEngine::PioPort::getAddrRanges() const
{
    return { AddrRange(engine.pioAddr, engine.pioAddr + engine.pioSize) };
}

// ---------------- SpmDmaEngine ----------------

SpmDmaEngine::SpmDmaEngine(const Params &p)
    : ClockedObject(p),
      system(p.system),
      pioPort(p.name + ".pio", this),
      dmaPort(this, p.system),
      pioAddr(p.pio_addr),
      pioSize(p.pio_size),
      pioDelay(p.pio_latency),
      initLatency(p.init_latency),
      state(Idle),
      srcAddr(0), dstAddr(0), totalLen(0),
      buffer(nullptr),
      pendingStatusPkt(nullptr),
      beginReadEvent([this]{ beginRead(); }, name() + ".beginRead"),
      readDoneEvent([this]{ readDone(); }, name() + ".readDone"),
      writeDoneEvent([this]{ writeDone(); }, name() + ".writeDone")
{
    panic_if(instance, "Only one SpmDmaEngine instance is supported");
    instance = this;
}

SpmDmaEngine::~SpmDmaEngine()
{
    delete[] buffer;
    instance = nullptr;
}

void
SpmDmaEngine::init()
{
    ClockedObject::init();
    if (pioPort.isConnected())
        pioPort.sendRangeChange();
}

Port &
SpmDmaEngine::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "pio")
        return pioPort;
    else if (if_name == "dma")
        return dmaPort;
    return ClockedObject::getPort(if_name, idx);
}

// ---------------- PIO register handlers ----------------

Tick
SpmDmaEngine::handleRead(PacketPtr pkt)
{
    Addr offset = pkt->getAddr() - pioAddr;
    uint64_t val = 0;

    switch (offset) {
      case REG_SRC:    val = srcAddr;  break;
      case REG_DST:    val = dstAddr;  break;
      case REG_LEN:    val = totalLen; break;
      case REG_STATUS: val = (state == Idle) ? 0 : 1; break;
      default:
        warn("SpmDmaEngine: read from unknown offset 0x%x\n", offset);
        break;
    }

    DPRINTF(SpmDma, "PIO read offset=0x%x val=0x%x\n", offset, val);
    pkt->setLE<uint64_t>(val);
    return pioDelay;
}

Tick
SpmDmaEngine::handleWrite(PacketPtr pkt)
{
    Addr offset = pkt->getAddr() - pioAddr;
    uint64_t val = pkt->getLE<uint64_t>();

    DPRINTF(SpmDma, "PIO write offset=0x%x val=0x%x\n", offset, val);

    switch (offset) {
      case REG_SRC:
        srcAddr = val;
        break;
      case REG_DST:
        dstAddr = val;
        break;
      case REG_LEN:
        startCopy(srcAddr, dstAddr, val);
        break;
      default:
        warn("SpmDmaEngine: write to unknown offset 0x%x\n", offset);
        break;
    }

    return pioDelay;
}

// ---------------- DMA logic ----------------

void
SpmDmaEngine::startCopy(Addr src, Addr dst, uint64_t len)
{
    panic_if(state != Idle,
             "SpmDmaEngine::startCopy called while busy (state=%d)", state);

    if (len == 0)
        return;

    srcAddr  = src;
    dstAddr  = dst;
    totalLen = len;
    buffer   = new uint8_t[len];
    state    = Reading;

    DPRINTF(SpmDma, "startCopy: src=0x%x dst=0x%x len=%d "
            "(init_latency=%d ticks)\n", src, dst, len, initLatency);

    schedule(beginReadEvent, curTick() + initLatency);
}

void
SpmDmaEngine::beginRead()
{
    dmaPort.dmaAction(MemCmd::ReadReq, srcAddr, totalLen,
                      &readDoneEvent, buffer, 0);
}

void
SpmDmaEngine::readDone()
{
    DPRINTF(SpmDma, "readDone: writing %d bytes to dst=0x%x\n",
            totalLen, dstAddr);

    state = Writing;
    dmaPort.dmaAction(MemCmd::WriteReq, dstAddr, totalLen,
                      &writeDoneEvent, buffer, 0);
}

void
SpmDmaEngine::writeDone()
{
    DPRINTF(SpmDma, "writeDone: transfer complete\n");
    transferComplete();
}

void
SpmDmaEngine::transferComplete()
{
    delete[] buffer;
    buffer = nullptr;
    state  = Idle;

    if (pendingStatusPkt) {
        uint64_t status = 0;
        pendingStatusPkt->setLE<uint64_t>(status);
        pioPort.schedTimingResp(pendingStatusPkt, curTick() + pioDelay);
        pendingStatusPkt = nullptr;
        DPRINTF(SpmDma, "Unblocked pending STATUS read\n");
    }
}

// ---------------- Free functions for ISA instructions ----------------

void
spmDmaStartCopy(Addr src, Addr dst, uint64_t len)
{
    auto *eng = SpmDmaEngine::getInstance();
    panic_if(!eng, "spmDmaStartCopy: no SpmDmaEngine instance");
    eng->startCopy(src, dst, len);
}

bool
spmDmaIsComplete()
{
    auto *eng = SpmDmaEngine::getInstance();
    panic_if(!eng, "spmDmaIsComplete: no SpmDmaEngine instance");
    return eng->isComplete();
}

} // namespace gem5
