#include "scratchpad_mem/scratchpad_memory.hh"

#include <algorithm>
#include <set>

#include "base/trace.hh"
#include "debug/Drain.hh"
#include "debug/ScratchpadMem.hh"

namespace gem5
{

namespace memory
{

ScratchpadMemory::ScratchpadMemory(const ScratchpadMemoryParams &p)
    : AbstractMemory(p),
      port(name() + ".port", *this),
      latency(p.latency),
      latencyVar(p.latency_var),
      bandwidth(p.bandwidth),
      numBanks(p.num_banks),
      bankIntlvSize(p.bank_interleave_size),
      bankBusyUntil(p.num_banks, 0),
      isBusy(false),
      retryReq(false),
      retryResp(false),
      releaseEvent([this]{ release(); }, name()),
      dequeueEvent([this]{ dequeue(); }, name()),
      spmStats(*this)
{
    panic_if(numBanks == 0, "num_banks must be > 0");
    panic_if(bankIntlvSize == 0, "bank_interleave_size must be > 0");
    panic_if((bankIntlvSize & (bankIntlvSize - 1)) != 0,
             "bank_interleave_size must be a power of 2");
}

void
ScratchpadMemory::init()
{
    AbstractMemory::init();

    if (port.isConnected()) {
        port.sendRangeChange();
    }

    DPRINTF(ScratchpadMem, "Initialized: %d banks, %d-byte interleave, "
            "%d tick base latency, size=%#x\n",
            numBanks, bankIntlvSize, latency, range.size());
}

unsigned
ScratchpadMemory::addrToBank(Addr addr) const
{
    return ((addr - range.start()) / bankIntlvSize) % numBanks;
}

Tick
ScratchpadMemory::getLatency() const
{
    return latency +
        (latencyVar ? rng->random<Tick>(0, latencyVar) : 0);
}

Tick
ScratchpadMemory::recvAtomic(PacketPtr pkt)
{
    panic_if(pkt->cacheResponding(),
             "Should not see packets where cache is responding");

    access(pkt);
    return getLatency();
}

Tick
ScratchpadMemory::recvAtomicBackdoor(PacketPtr pkt, MemBackdoorPtr &_backdoor)
{
    Tick lat = recvAtomic(pkt);
    getBackdoor(_backdoor);
    return lat;
}

void
ScratchpadMemory::recvFunctional(PacketPtr pkt)
{
    pkt->pushLabel(name());

    functionalAccess(pkt);

    bool done = false;
    auto p = packetQueue.begin();
    while (!done && p != packetQueue.end()) {
        done = pkt->trySatisfyFunctional(p->pkt);
        ++p;
    }

    pkt->popLabel();
}

void
ScratchpadMemory::recvMemBackdoorReq(const MemBackdoorReq &req,
        MemBackdoorPtr &_backdoor)
{
    getBackdoor(_backdoor);
}

bool
ScratchpadMemory::recvTimingReq(PacketPtr pkt)
{
    panic_if(pkt->cacheResponding(),
             "Should not see packets where cache is responding");

    panic_if(!(pkt->isRead() || pkt->isWrite()),
             "Should only see read and writes at memory controller, "
             "saw %s to %#llx\n", pkt->cmdString(), pkt->getAddr());

    if (retryReq)
        return false;

    if (isBusy) {
        retryReq = true;
        return false;
    }

    Tick receive_delay = pkt->headerDelay + pkt->payloadDelay;
    pkt->headerDelay = pkt->payloadDelay = 0;

    // Port-level bandwidth limiting
    Tick duration = pkt->getSize() * bandwidth;
    if (duration != 0) {
        schedule(releaseEvent, curTick() + duration);
        isBusy = true;
    }

    // --- Bank conflict modeling ---
    const Addr addr = pkt->getAddr();
    const unsigned size = pkt->getSize();
    const bool isRead = pkt->isRead();
    const Addr offset = addr - range.start();

    const unsigned startBlock = offset / bankIntlvSize;
    const unsigned endBlock = (offset + size - 1) / bankIntlvSize;

    std::set<unsigned> touchedBanks;
    for (unsigned b = startBlock; b <= endBlock; ++b)
        touchedBanks.insert(b % numBanks);

    Tick maxBankReady = curTick();
    bool hadConflict = false;

    for (unsigned bank : touchedBanks) {
        if (bankBusyUntil[bank] > curTick()) {
            hadConflict = true;
            spmStats.perBankConflicts[bank]++;
        }
        maxBankReady = std::max(maxBankReady, bankBusyUntil[bank]);

        if (isRead)
            spmStats.perBankReads[bank]++;
        else
            spmStats.perBankWrites[bank]++;
    }

    if (hadConflict)
        spmStats.bankConflicts++;

    const Tick accessLat = getLatency();
    const Tick effectiveDone = maxBankReady + accessLat;
    const Tick totalLatency = effectiveDone - curTick();

    for (unsigned bank : touchedBanks)
        bankBusyUntil[bank] = effectiveDone;

    if (isRead) {
        spmStats.reads++;
        spmStats.readLatency += totalLatency;
    } else {
        spmStats.writes++;
        spmStats.writeLatency += totalLatency;
    }

    DPRINTF(ScratchpadMem, "%s addr=%#x size=%d nBanks=%d latency=%d%s\n",
            isRead ? "Read" : "Write", addr, size,
            touchedBanks.size(), totalLatency,
            hadConflict ? " CONFLICT" : "");

    // Perform the data access (AbstractMemory updates base stats)
    bool needsResponse = pkt->needsResponse();
    access(pkt);

    if (needsResponse) {
        assert(pkt->isResponse());

        Tick when_to_send = curTick() + receive_delay + totalLatency;

        // Insertion sort by send time, preserving order for same address
        if (packetQueue.empty()) {
            packetQueue.emplace_back(pkt, when_to_send);
        } else {
            auto i = packetQueue.end();
            --i;
            while (i != packetQueue.begin() && when_to_send < i->tick &&
                   !i->pkt->matchAddr(pkt))
                --i;
            packetQueue.emplace(++i, pkt, when_to_send);
        }

        if (!retryResp && !dequeueEvent.scheduled())
            schedule(dequeueEvent, packetQueue.back().tick);
    } else {
        pendingDelete.reset(pkt);
    }

    return true;
}

void
ScratchpadMemory::release()
{
    assert(isBusy);
    isBusy = false;
    if (retryReq) {
        retryReq = false;
        port.sendRetryReq();
    }
}

void
ScratchpadMemory::dequeue()
{
    assert(!packetQueue.empty());
    DeferredPacket deferred_pkt = packetQueue.front();

    retryResp = !port.sendTimingResp(deferred_pkt.pkt);

    if (!retryResp) {
        packetQueue.pop_front();

        if (!packetQueue.empty()) {
            reschedule(dequeueEvent,
                       std::max(packetQueue.front().tick, curTick()), true);
        } else if (drainState() == DrainState::Draining) {
            DPRINTF(Drain, "Draining of ScratchpadMemory complete\n");
            signalDrainDone();
        }
    }
}

void
ScratchpadMemory::recvRespRetry()
{
    assert(retryResp);
    dequeue();
}

Port &
ScratchpadMemory::getPort(const std::string &if_name, PortID idx)
{
    if (if_name != "port") {
        return AbstractMemory::getPort(if_name, idx);
    } else {
        return port;
    }
}

DrainState
ScratchpadMemory::drain()
{
    if (!packetQueue.empty()) {
        DPRINTF(Drain,
                "ScratchpadMemory queue has requests, waiting to drain\n");
        return DrainState::Draining;
    } else {
        return DrainState::Drained;
    }
}

// --- MemoryPort ---

ScratchpadMemory::MemoryPort::MemoryPort(const std::string& _name,
                                         ScratchpadMemory& _memory)
    : ResponsePort(_name), mem(_memory)
{ }

AddrRangeList
ScratchpadMemory::MemoryPort::getAddrRanges() const
{
    AddrRangeList ranges;
    ranges.push_back(mem.getAddrRange());
    return ranges;
}

Tick
ScratchpadMemory::MemoryPort::recvAtomic(PacketPtr pkt)
{
    return mem.recvAtomic(pkt);
}

Tick
ScratchpadMemory::MemoryPort::recvAtomicBackdoor(
        PacketPtr pkt, MemBackdoorPtr &_backdoor)
{
    return mem.recvAtomicBackdoor(pkt, _backdoor);
}

void
ScratchpadMemory::MemoryPort::recvFunctional(PacketPtr pkt)
{
    mem.recvFunctional(pkt);
}

void
ScratchpadMemory::MemoryPort::recvMemBackdoorReq(const MemBackdoorReq &req,
        MemBackdoorPtr &backdoor)
{
    mem.recvMemBackdoorReq(req, backdoor);
}

bool
ScratchpadMemory::MemoryPort::recvTimingReq(PacketPtr pkt)
{
    return mem.recvTimingReq(pkt);
}

void
ScratchpadMemory::MemoryPort::recvRespRetry()
{
    mem.recvRespRetry();
}

// --- Stats ---

ScratchpadMemory::SpmStats::SpmStats(ScratchpadMemory &_spm)
    : statistics::Group(&_spm),
      spm(_spm),
      ADD_STAT(reads, statistics::units::Count::get(),
               "Total SPM read accesses"),
      ADD_STAT(writes, statistics::units::Count::get(),
               "Total SPM write accesses"),
      ADD_STAT(readLatency, statistics::units::Tick::get(),
               "Cumulative read latency"),
      ADD_STAT(writeLatency, statistics::units::Tick::get(),
               "Cumulative write latency"),
      ADD_STAT(avgReadLatency, statistics::units::Tick::get(),
               "Average read latency per access"),
      ADD_STAT(avgWriteLatency, statistics::units::Tick::get(),
               "Average write latency per access"),
      ADD_STAT(bankConflicts, statistics::units::Count::get(),
               "Total bank conflict events"),
      ADD_STAT(perBankReads, statistics::units::Count::get(),
               "Per-bank read access count"),
      ADD_STAT(perBankWrites, statistics::units::Count::get(),
               "Per-bank write access count"),
      ADD_STAT(perBankConflicts, statistics::units::Count::get(),
               "Per-bank conflict count")
{
}

void
ScratchpadMemory::SpmStats::regStats()
{
    statistics::Group::regStats();

    using namespace statistics;

    perBankReads.init(spm.numBanks);
    perBankWrites.init(spm.numBanks);
    perBankConflicts.init(spm.numBanks);

    for (unsigned i = 0; i < spm.numBanks; i++) {
        perBankReads.subname(i, csprintf("bank%d", i));
        perBankWrites.subname(i, csprintf("bank%d", i));
        perBankConflicts.subname(i, csprintf("bank%d", i));
    }

    avgReadLatency = readLatency / reads;
    avgWriteLatency = writeLatency / writes;
}

} // namespace memory
} // namespace gem5
