#include "scratchpad_mem/spm_dma_engine.hh"

#include "base/trace.hh"
#include "cpu/thread_context.hh"
#include "debug/SpmDma.hh"
#include "mem/packet_access.hh"
#include "sim/system.hh"

namespace gem5
{

// ---- Per-System registry (replaces singleton) ----

std::unordered_map<System*, SpmDmaEngine*> SpmDmaEngine::registry;

SpmDmaEngine *
SpmDmaEngine::lookup(System *sys)
{
    auto it = registry.find(sys);
    return (it != registry.end()) ? it->second : nullptr;
}

// ---- PioPort ----

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

    // STATUS read: polling semantics — return the current pending count
    // immediately so the CPU can branch-poll without blocking.
    if (pkt->isRead() && offset == REG_STATUS) {
        uint64_t pending = engine.pendingCount();

        if (pending > 0) {
            // DMA still busy — record busy-poll for stall tracking.
            engine.dmaStats.waitPollBusy++;
            if (engine.waitStartTick == 0)
                engine.waitStartTick = curTick();
        } else {
            // DMA idle — close the wait-stall window if one was open.
            engine.dmaStats.waitPollIdle++;
            if (engine.waitStartTick != 0) {
                engine.dmaStats.waitStallCycles +=
                    curTick() - engine.waitStartTick;
                engine.waitStartTick = 0;
            }
        }

        DPRINTF(SpmDma, "STATUS poll: pending=%d\n", pending);
    }

    // Fall through to the normal SimpleTimingPort path which calls
    // recvAtomic → handleRead/handleWrite and schedules the response.
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

// ---- SpmDmaEngine ----

SpmDmaEngine::SpmDmaEngine(const Params &p)
    : ClockedObject(p),
      system(p.system),
      pioPort(p.name + ".pio", this),
      dmaPort(this, p.system),
      pioAddr(p.pio_addr),
      pioSize(p.pio_size),
      pioDelay(p.pio_latency),
      descLatency(p.desc_latency),
      maxDescriptors(p.max_descriptors),
      state(Idle),
      curSrc(0), curDst(0), curLen(0),
      buffer(nullptr),
      stagedSrc(0), stagedDst(0),
      transferStartTick(0),
      waitStartTick(0),
      beginReadEvent([this]{ beginRead(); }, name() + ".beginRead"),
      readDoneEvent([this]{ readDone(); }, name() + ".readDone"),
      writeDoneEvent([this]{ writeDone(); }, name() + ".writeDone"),
      dmaStats(*this)
{
    panic_if(maxDescriptors == 0, "max_descriptors must be > 0");
    auto [it, inserted] = registry.emplace(system, this);
    panic_if(!inserted,
             "SpmDmaEngine: a DMA engine is already registered "
             "for this System (multi-core requires per-CPU keying)");
}

SpmDmaEngine::~SpmDmaEngine()
{
    delete[] buffer;
    registry.erase(system);
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

// ---- PIO register handlers ----

Tick
SpmDmaEngine::handleRead(PacketPtr pkt)
{
    Addr offset = pkt->getAddr() - pioAddr;
    uint64_t val = 0;

    switch (offset) {
      case REG_SRC:    val = stagedSrc;      break;
      case REG_DST:    val = stagedDst;      break;
      case REG_LEN:    val = curLen;          break;
      case REG_STATUS: val = pendingCount();  break;
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
        stagedSrc = val;
        break;
      case REG_DST:
        stagedDst = val;
        break;
      case REG_LEN:
        if (!startCopy(stagedSrc, stagedDst, val))
            warn("SpmDmaEngine: descriptor queue full, transfer dropped\n");
        break;
      default:
        warn("SpmDmaEngine: write to unknown offset 0x%x\n", offset);
        break;
    }

    return pioDelay;
}

// ---- Descriptor queue + DMA logic ----

bool
SpmDmaEngine::startCopy(Addr src, Addr dst, uint64_t len)
{
    if (len == 0)
        return true;

    if (descQueue.size() >= maxDescriptors) {
        dmaStats.queueFullStalls++;
        DPRINTF(SpmDma, "Queue full (%d/%d), rejecting src=0x%x dst=0x%x "
                "len=%d\n", descQueue.size(), maxDescriptors, src, dst, len);
        return false;
    }

    descQueue.push_back({src, dst, len});

    DPRINTF(SpmDma, "Enqueued: src=0x%x dst=0x%x len=%d "
            "(queue depth: %d/%d)\n",
            src, dst, len, descQueue.size(), maxDescriptors);

    if (state == Idle)
        startNextTransfer();

    return true;
}

void
SpmDmaEngine::startNextTransfer()
{
    assert(state == Idle);
    assert(!descQueue.empty());

    Descriptor desc = descQueue.front();
    descQueue.pop_front();

    curSrc = desc.src;
    curDst = desc.dst;
    curLen = desc.len;
    buffer = new uint8_t[curLen];
    state  = Reading;
    transferStartTick = curTick();

    DPRINTF(SpmDma, "Starting transfer: src=0x%x dst=0x%x len=%d "
            "(desc_latency=%d ticks)\n",
            curSrc, curDst, curLen, descLatency);

    schedule(beginReadEvent, curTick() + descLatency);
}

void
SpmDmaEngine::beginRead()
{
    dmaPort.dmaAction(MemCmd::ReadReq, curSrc, curLen,
                      &readDoneEvent, buffer, 0);
}

void
SpmDmaEngine::readDone()
{
    DPRINTF(SpmDma, "readDone: writing %d bytes to dst=0x%x\n",
            curLen, curDst);

    state = Writing;
    dmaPort.dmaAction(MemCmd::WriteReq, curDst, curLen,
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
    dmaStats.transfers++;
    dmaStats.bytesTransferred += curLen;
    dmaStats.busyTicks += curTick() - transferStartTick;

    DPRINTF(SpmDma, "Transfer complete: %d bytes, latency=%d ticks "
            "(remaining in queue: %d)\n",
            curLen, curTick() - transferStartTick,
            (unsigned)descQueue.size());

    delete[] buffer;
    buffer = nullptr;
    state  = Idle;

    // Start next queued transfer if any.
    if (!descQueue.empty()) {
        startNextTransfer();
        return;
    }

    // All transfers done — the next STATUS poll will see pendingCount()==0
    // and close the wait-stall window.  No need to actively unblock
    // anything; the polling CPU will observe idle on its next iteration.

    if (drainState() == DrainState::Draining) {
        DPRINTF(SpmDma, "Drain complete\n");
        signalDrainDone();
    }
}

// ---- Drain ----

DrainState
SpmDmaEngine::drain()
{
    if (!isComplete()) {
        DPRINTF(SpmDma, "DMA busy (state=%d, queue=%d), waiting to drain\n",
                state, (unsigned)descQueue.size());
        return DrainState::Draining;
    }
    return DrainState::Drained;
}

// ---- Stats ----

SpmDmaEngine::DmaStats::DmaStats(SpmDmaEngine &_engine)
    : statistics::Group(&_engine),
      ADD_STAT(transfers, statistics::units::Count::get(),
               "Total DMA transfers completed"),
      ADD_STAT(bytesTransferred, statistics::units::Byte::get(),
               "Total bytes transferred by DMA"),
      ADD_STAT(busyTicks, statistics::units::Tick::get(),
               "Total ticks DMA engine was busy"),
      ADD_STAT(avgLatency, statistics::units::Tick::get(),
               "Average latency per DMA transfer"),
      ADD_STAT(queueFullStalls, statistics::units::Count::get(),
               "Enqueue attempts rejected due to full descriptor queue"),
      ADD_STAT(waitPollBusy, statistics::units::Count::get(),
               "STATUS polls that found DMA engine busy"),
      ADD_STAT(waitPollIdle, statistics::units::Count::get(),
               "STATUS polls that found DMA engine idle (wait complete)"),
      ADD_STAT(waitStallCycles, statistics::units::Tick::get(),
               "Total stall cycles across all spm.dma.w wait sequences"),
      ADD_STAT(avgWaitStallCycles, statistics::units::Tick::get(),
               "Average stall cycles per spm.dma.w wait sequence")
{
}

void
SpmDmaEngine::DmaStats::regStats()
{
    statistics::Group::regStats();
    avgLatency = busyTicks / transfers;
    avgWaitStallCycles = waitStallCycles / waitPollIdle;
}

// ---- Free functions for ISA instructions ----

bool
spmDmaStartCopy(ThreadContext *tc, Addr src, Addr dst, uint64_t len)
{
    auto *eng = SpmDmaEngine::lookup(tc->getSystemPtr());
    panic_if(!eng, "spmDmaStartCopy: no SpmDmaEngine registered "
             "for this System");
    return eng->startCopy(src, dst, len);
}

bool
spmDmaIsComplete(ThreadContext *tc)
{
    auto *eng = SpmDmaEngine::lookup(tc->getSystemPtr());
    panic_if(!eng, "spmDmaIsComplete: no SpmDmaEngine registered "
             "for this System");
    return eng->isComplete();
}

bool
spmDmaQueueFull(ThreadContext *tc)
{
    auto *eng = SpmDmaEngine::lookup(tc->getSystemPtr());
    panic_if(!eng, "spmDmaQueueFull: no SpmDmaEngine registered "
             "for this System");
    return eng->queueFull();
}

} // namespace gem5
