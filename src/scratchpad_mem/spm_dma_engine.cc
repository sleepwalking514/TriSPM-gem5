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
                    engine.ticksToCycles(curTick() - engine.waitStartTick);
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
      curHeight(1), curSrcStride(0), curDstStride(0),
      rowsWriteComplete(0),
      stagedSrc(0), stagedDst(0),
      stagedSrcStride(0), stagedDstStride(0), stagedHeight(0),
      transferStartTick(0),
      waitStartTick(0),
      beginReadEvent([this]{ beginRead(); }, name() + ".beginRead"),
      readDoneEvent([this]{ readDone(); }, name() + ".readDone"),
      writeDoneEvent([this]{ writeDone(); }, name() + ".writeDone"),
      beginPipelinedReadEvent([this]{ beginPipelinedRead(); },
                               name() + ".beginPipelinedRead"),
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
    cleanupPipelinedState();
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
      case REG_SRC:        val = stagedSrc;         break;
      case REG_DST:        val = stagedDst;         break;
      case REG_LEN:        val = curLen;             break;
      case REG_STATUS:     val = pendingCount();     break;
      case REG_SRC_STRIDE: val = stagedSrcStride;   break;
      case REG_DST_STRIDE: val = stagedDstStride;   break;
      case REG_HEIGHT:     val = stagedHeight;       break;
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
      case REG_SRC_STRIDE:
        stagedSrcStride = val;
        break;
      case REG_DST_STRIDE:
        stagedDstStride = val;
        break;
      case REG_HEIGHT:
        stagedHeight = (uint32_t)val;
        break;
      case REG_LEN:
        if (!startCopy(stagedSrc, stagedDst, val,
                       stagedSrcStride, stagedDstStride, stagedHeight))
            warn("SpmDmaEngine: descriptor queue full, transfer dropped\n");
        // Reset staged 2D fields after enqueue so next 1D transfer
        // doesn't accidentally inherit them.
        stagedSrcStride = 0;
        stagedDstStride = 0;
        stagedHeight = 0;
        break;
      default:
        warn("SpmDmaEngine: write to unknown offset 0x%x\n", offset);
        break;
    }

    return pioDelay;
}

// ---- Descriptor queue + DMA logic ----

bool
SpmDmaEngine::startCopy(Addr src, Addr dst, uint64_t len,
                         uint64_t srcStride, uint64_t dstStride,
                         uint32_t height)
{
    if (len == 0)
        return true;

    if (descQueue.size() >= maxDescriptors) {
        dmaStats.queueFullStalls++;
        DPRINTF(SpmDma, "Queue full (%d/%d), rejecting src=0x%x dst=0x%x "
                "len=%d\n", descQueue.size(), maxDescriptors, src, dst, len);
        return false;
    }

    descQueue.push_back({src, dst, len, srcStride, dstStride, height});

    bool is2D = height > 1;
    if (is2D) {
        DPRINTF(SpmDma, "Enqueued 2D: src=0x%x dst=0x%x width=%d height=%d "
                "srcStride=%d dstStride=%d (queue depth: %d/%d)\n",
                src, dst, len, height,
                srcStride, dstStride,
                descQueue.size(), maxDescriptors);
    } else {
        DPRINTF(SpmDma, "Enqueued 1D: src=0x%x dst=0x%x len=%d "
                "(queue depth: %d/%d)\n",
                src, dst, len, descQueue.size(), maxDescriptors);
    }

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
    curSrcStride = desc.srcStride;
    curDstStride = desc.dstStride;
    curHeight = (desc.height > 1) ? desc.height : 1;
    state  = Reading;
    transferStartTick = curTick();

    if (curHeight > 1) {
        // --- 2D pipelined path ---
        DPRINTF(SpmDma, "Starting 2D pipelined transfer: src=0x%x dst=0x%x "
                "width=%d height=%d srcStride=%d dstStride=%d "
                "(desc_latency=%d ticks)\n",
                curSrc, curDst, curLen, curHeight,
                curSrcStride, curDstStride, descLatency);

        // Allocate per-row buffers and events
        rowsWriteComplete = 0;
        rowBuffers.resize(curHeight);
        rowReadDoneEvents.resize(curHeight);
        rowWriteDoneEvents.resize(curHeight);

        for (uint32_t r = 0; r < curHeight; r++) {
            rowBuffers[r] = new uint8_t[curLen];
            rowReadDoneEvents[r] = new EventFunctionWrapper(
                [this, r]{ pipelinedRowReadDone(r); },
                name() + ".rowReadDone");
            rowWriteDoneEvents[r] = new EventFunctionWrapper(
                [this, r]{ pipelinedRowWriteDone(r); },
                name() + ".rowWriteDone");
        }

        schedule(beginPipelinedReadEvent, curTick() + descLatency);
    } else {
        // --- 1D path (unchanged) ---
        buffer = new uint8_t[curLen];

        DPRINTF(SpmDma, "Starting 1D transfer: src=0x%x dst=0x%x len=%d "
                "(desc_latency=%d ticks)\n",
                curSrc, curDst, curLen, descLatency);

        schedule(beginReadEvent, curTick() + descLatency);
    }
}

// ---- 1D path (unchanged from original) ----

void
SpmDmaEngine::beginRead()
{
    DPRINTF(SpmDma, "beginRead: 1D, src=0x%x, %d bytes\n", curSrc, curLen);
    dmaPort.dmaAction(MemCmd::ReadReq, curSrc, curLen,
                      &readDoneEvent, buffer, 0);
}

void
SpmDmaEngine::readDone()
{
    DPRINTF(SpmDma, "readDone: 1D, writing %d bytes to dst=0x%x\n",
            curLen, curDst);
    state = Writing;
    dmaPort.dmaAction(MemCmd::WriteReq, curDst, curLen,
                      &writeDoneEvent, buffer, 0);
}

void
SpmDmaEngine::writeDone()
{
    DPRINTF(SpmDma, "writeDone: 1D complete\n");
    dmaStats.rowsTransferred++;
    dmaStats.bytesTransferred += curLen;
    transferComplete();
}

// ---- 2D pipelined path ----

void
SpmDmaEngine::beginPipelinedRead()
{
    DPRINTF(SpmDma, "beginPipelinedRead: issuing %d row reads in parallel, "
            "%d bytes each\n", curHeight, curLen);

    // Issue all row reads concurrently — DmaPort queues them internally
    // and pipelines the memory accesses.
    for (uint32_t r = 0; r < curHeight; r++) {
        Addr rowSrc = curSrc + (uint64_t)r * curSrcStride;
        DPRINTF(SpmDma, "  row %d: read src=0x%x\n", r, rowSrc);
        dmaPort.dmaAction(MemCmd::ReadReq, rowSrc, curLen,
                          rowReadDoneEvents[r], rowBuffers[r], 0);
    }
}

void
SpmDmaEngine::pipelinedRowReadDone(int row)
{
    // This row's read is complete — immediately start writing it.
    Addr rowDst = curDst + (uint64_t)row * curDstStride;
    DPRINTF(SpmDma, "pipelinedRowReadDone: row %d/%d, writing %d bytes "
            "to dst=0x%x\n", row, curHeight, curLen, rowDst);

    dmaPort.dmaAction(MemCmd::WriteReq, rowDst, curLen,
                      rowWriteDoneEvents[row], rowBuffers[row], 0);
}

void
SpmDmaEngine::pipelinedRowWriteDone(int row)
{
    DPRINTF(SpmDma, "pipelinedRowWriteDone: row %d/%d complete\n",
            row, curHeight);

    dmaStats.rowsTransferred++;
    dmaStats.bytesTransferred += curLen;
    rowsWriteComplete++;

    if ((uint32_t)rowsWriteComplete >= curHeight) {
        // All rows done — clean up and complete the transfer.
        cleanupPipelinedState();
        transferComplete();
    }
}

void
SpmDmaEngine::cleanupPipelinedState()
{
    for (uint32_t r = 0; r < rowBuffers.size(); r++) {
        delete[] rowBuffers[r];
        delete rowReadDoneEvents[r];
        delete rowWriteDoneEvents[r];
    }
    rowBuffers.clear();
    rowReadDoneEvents.clear();
    rowWriteDoneEvents.clear();
}

void
SpmDmaEngine::transferComplete()
{
    dmaStats.transfers++;
    if (curHeight > 1)
        dmaStats.transfers2D++;
    dmaStats.busyCycles += ticksToCycles(curTick() - transferStartTick);

    DPRINTF(SpmDma, "Transfer complete: %d bytes (%d rows x %d), "
            "latency=%d ticks (remaining in queue: %d)\n",
            (uint64_t)curLen * curHeight, curHeight, curLen,
            curTick() - transferStartTick,
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
               "Total DMA transfers completed (1D + 2D descriptors)"),
      ADD_STAT(transfers2D, statistics::units::Count::get(),
               "Total 2D DMA transfers completed"),
      ADD_STAT(rowsTransferred, statistics::units::Count::get(),
               "Total rows transferred (1D counts as 1 row each)"),
      ADD_STAT(bytesTransferred, statistics::units::Byte::get(),
               "Total bytes transferred by DMA"),
      ADD_STAT(busyCycles, statistics::units::Cycle::get(),
               "Total cycles DMA engine was busy"),
      ADD_STAT(avgLatency, statistics::units::Cycle::get(),
               "Average cycles per DMA transfer"),
      ADD_STAT(queueFullStalls, statistics::units::Count::get(),
               "Enqueue attempts rejected due to full descriptor queue"),
      ADD_STAT(waitPollBusy, statistics::units::Count::get(),
               "STATUS polls that found DMA engine busy"),
      ADD_STAT(waitPollIdle, statistics::units::Count::get(),
               "STATUS polls that found DMA engine idle (wait complete)"),
      ADD_STAT(waitStallCycles, statistics::units::Cycle::get(),
               "Total stall cycles across all spm.dma.w wait sequences"),
      ADD_STAT(avgWaitStallCycles, statistics::units::Cycle::get(),
               "Average stall cycles per spm.dma.w wait sequence")
{
}

void
SpmDmaEngine::DmaStats::regStats()
{
    statistics::Group::regStats();
    avgLatency = busyCycles / transfers;
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
spmDmaStartCopy2D(ThreadContext *tc, Addr src, Addr dst,
                   uint64_t width, uint32_t height,
                   uint64_t srcStride, uint64_t dstStride)
{
    auto *eng = SpmDmaEngine::lookup(tc->getSystemPtr());
    panic_if(!eng, "spmDmaStartCopy2D: no SpmDmaEngine registered "
             "for this System");
    return eng->startCopy(src, dst, width, srcStride, dstStride, height);
}

void
spmDmaSetStride(ThreadContext *tc, uint64_t srcStride, uint64_t dstStride)
{
    auto *eng = SpmDmaEngine::lookup(tc->getSystemPtr());
    panic_if(!eng, "spmDmaSetStride: no SpmDmaEngine registered "
             "for this System");
    eng->setStride(srcStride, dstStride);
}

bool
spmDmaStartCopy2DStaged(ThreadContext *tc, Addr src, Addr dst,
                         uint64_t width, uint32_t height)
{
    auto *eng = SpmDmaEngine::lookup(tc->getSystemPtr());
    panic_if(!eng, "spmDmaStartCopy2DStaged: no SpmDmaEngine registered "
             "for this System");
    uint64_t srcStr = eng->getStagedSrcStride();
    uint64_t dstStr = eng->getStagedDstStride();
    return eng->startCopy(src, dst, width, srcStr, dstStr, height);
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
