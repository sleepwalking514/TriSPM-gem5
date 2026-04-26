#include "scratchpad_mem/spm_dma_engine.hh"

#include "base/trace.hh"
#include "cpu/thread_context.hh"
#include "debug/SpmDma.hh"
#include "mem/packet_access.hh"
#include "mem/page_table.hh"
#include "sim/process.hh"
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
      stagedSrcStride(0), stagedDstStride(0), stagedHeight(1),
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
        stagedHeight = 1;
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
    curHeight = desc.height;
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

// ---- SE-mode VA→PA translation ----

Addr
SpmDmaEngine::translateAddr(Addr vaddr)
{
    auto *tc = system->threads[0];
    auto *process = tc->getProcessPtr();
    Addr paddr;
    if (!process->pTable->translate(vaddr, paddr))
        panic("SpmDmaEngine: no page table entry for VA 0x%x\n", vaddr);
    return paddr;
}

// Split a DMA action that spans multiple VA pages into one sub-action per
// page, because gem5 SE mode allocates physical pages from a free pool and
// consecutive VAs are not guaranteed to map to consecutive PAs.  Each
// sub-action drives the same logical `doneEvent`; the last one to finish
// schedules it.
void
SpmDmaEngine::issuePagedDmaAction(Packet::Command cmd, Addr vaddr,
                                   uint64_t len,
                                   EventFunctionWrapper *doneEvent,
                                   uint8_t *buf)
{
    constexpr uint64_t PAGE_SIZE = 4096;

    // Compute (PA, chunkLen, bufOffset) for each VA-page-bounded chunk.
    struct Chunk { Addr pa; uint64_t bytes; uint64_t bufOff; };
    std::vector<Chunk> chunks;
    uint64_t off = 0;
    while (off < len) {
        Addr va = vaddr + off;
        uint64_t pageEnd = (va & ~(PAGE_SIZE - 1)) + PAGE_SIZE;
        uint64_t chunkLen = std::min(pageEnd - va, len - off);
        chunks.push_back({translateAddr(va), chunkLen, off});
        off += chunkLen;
    }

    // Fast path: single chunk → reuse the original event directly.
    if (chunks.size() == 1) {
        dmaPort.dmaAction(cmd, chunks[0].pa, chunks[0].bytes,
                          doneEvent, buf + chunks[0].bufOff, 0);
        return;
    }

    // Multi-chunk: each sub-action gets its own auto-deleting event that
    // decrements a shared counter; the last sub-action schedules doneEvent.
    int *remaining = new int(chunks.size());
    DPRINTF(SpmDma, "VA 0x%x len=%d crosses %d pages — splitting\n",
            vaddr, len, (int)chunks.size());

    for (auto &c : chunks) {
        auto *subEvent = new EventFunctionWrapper(
            [this, remaining, doneEvent]() {
                if (--(*remaining) == 0) {
                    delete remaining;
                    schedule(doneEvent, curTick());
                }
            },
            name() + ".subDmaAction",
            /*del=*/true);
        dmaPort.dmaAction(cmd, c.pa, c.bytes, subEvent, buf + c.bufOff, 0);
    }
}

// ---- 1D path ----

void
SpmDmaEngine::beginRead()
{
    DPRINTF(SpmDma, "beginRead: 1D, src VA=0x%x, %d bytes\n",
            curSrc, curLen);
    issuePagedDmaAction(MemCmd::ReadReq, curSrc, curLen,
                        &readDoneEvent, buffer);
}

void
SpmDmaEngine::readDone()
{
    DPRINTF(SpmDma, "readDone: 1D, writing %d bytes to dst VA=0x%x "
            "first4B=0x%08x\n",
            curLen, curDst,
            curLen >= 4 ? *(uint32_t *)buffer : 0);
    state = Writing;
    issuePagedDmaAction(MemCmd::WriteReq, curDst, curLen,
                        &writeDoneEvent, buffer);
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
    // and pipelines the memory accesses.  Each row may itself span
    // multiple VA pages, so issuePagedDmaAction further splits as needed.
    for (uint32_t r = 0; r < curHeight; r++) {
        Addr rowSrc = curSrc + (uint64_t)r * curSrcStride;
        DPRINTF(SpmDma, "  row %d: read src VA=0x%x len=%d\n",
                r, rowSrc, curLen);
        issuePagedDmaAction(MemCmd::ReadReq, rowSrc, curLen,
                            rowReadDoneEvents[r], rowBuffers[r]);
    }
}

void
SpmDmaEngine::pipelinedRowReadDone(int row)
{
    // This row's read is complete — immediately start writing it.
    Addr rowDst = curDst + (uint64_t)row * curDstStride;
    DPRINTF(SpmDma, "pipelinedRowReadDone: row %d/%d, writing %d bytes "
            "to dst VA=0x%x\n", row, curHeight, curLen, rowDst);

    issuePagedDmaAction(MemCmd::WriteReq, rowDst, curLen,
                        rowWriteDoneEvents[row], rowBuffers[row]);
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
