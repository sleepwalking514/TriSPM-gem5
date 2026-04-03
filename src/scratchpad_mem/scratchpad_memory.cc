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
      port(name() + ".port", *this, PORT_BUS),
      cpuPort(name() + ".cpu_port", *this, PORT_CPU),
      latency(p.latency),
      latencyVar(p.latency_var),
      bandwidth(p.bandwidth),
      numBanks(p.num_banks),
      bankIntlvSize(p.bank_interleave_size),
      bankBusyUntil(p.num_banks, std::array<Tick, NUM_PORTS>{{0, 0}}),
      isBusy_{false, false},
      retryReq_{false, false},
      retryResp_{false, false},
      busReleaseEvent([this]{ release(PORT_BUS); }, name()),
      cpuReleaseEvent([this]{ release(PORT_CPU); }, name()),
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

    if (port.isConnected())
        port.sendRangeChange();
    if (cpuPort.isConnected())
        cpuPort.sendRangeChange();

    DPRINTF(ScratchpadMem, "Initialized (dual-port): %d banks, %d-byte interleave, "
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
ScratchpadMemory::recvTimingReq(PacketPtr pkt, int portId)
{
    panic_if(pkt->cacheResponding(),
             "Should not see packets where cache is responding");

    panic_if(!(pkt->isRead() || pkt->isWrite()),
             "Should only see read and writes at memory controller, "
             "saw %s to %#llx\n", pkt->cmdString(), pkt->getAddr());

    if (retryReq_[portId])
        return false;

    if (isBusy_[portId]) {
        retryReq_[portId] = true;
        return false;
    }

    Tick receive_delay = pkt->headerDelay + pkt->payloadDelay;
    pkt->headerDelay = pkt->payloadDelay = 0;

    Tick duration = pkt->getSize() * bandwidth;
    if (duration != 0) {
        auto &relEvent = (portId == PORT_CPU) ? cpuReleaseEvent
                                              : busReleaseEvent;
        schedule(relEvent, curTick() + duration);
        isBusy_[portId] = true;
    }

    // --- Bank conflict modeling (dual-port SRAM) ---
    // Each bank has two independent ports; a conflict only occurs when
    // the *same* port re-accesses a bank before its prior access completes.
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
        if (bankBusyUntil[bank][portId] > curTick()) {
            hadConflict = true;
            spmStats.perBankConflicts[bank]++;
        }
        maxBankReady = std::max(maxBankReady,
                                bankBusyUntil[bank][portId]);

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
        bankBusyUntil[bank][portId] = effectiveDone;

    if (isRead) {
        spmStats.reads++;
        spmStats.readLatency += totalLatency;
    } else {
        spmStats.writes++;
        spmStats.writeLatency += totalLatency;
    }

    DPRINTF(ScratchpadMem, "[port%d] %s addr=%#x size=%d nBanks=%d "
            "latency=%d%s\n",
            portId, isRead ? "Read" : "Write", addr, size,
            touchedBanks.size(), totalLatency,
            hadConflict ? " CONFLICT" : "");

    bool needsResponse = pkt->needsResponse();
    access(pkt);

    if (needsResponse) {
        assert(pkt->isResponse());

        Tick when_to_send = curTick() + receive_delay + totalLatency;

        if (packetQueue.empty()) {
            packetQueue.emplace_back(pkt, when_to_send, portId);
        } else {
            auto i = packetQueue.end();
            --i;
            while (i != packetQueue.begin() && when_to_send < i->tick &&
                   !i->pkt->matchAddr(pkt))
                --i;
            packetQueue.emplace(++i, pkt, when_to_send, portId);
        }

        // Only schedule the dequeue event if NO port is waiting for a
        // response retry.  The packet queue is shared, so the front packet
        // might belong to a different port that is currently blocked.
        bool anyRetry = false;
        for (int p = 0; p < NUM_PORTS; p++)
            anyRetry |= retryResp_[p];

        if (!anyRetry && !dequeueEvent.scheduled())
            schedule(dequeueEvent, packetQueue.back().tick);
    } else {
        pendingDelete.reset(pkt);
    }

    return true;
}

void
ScratchpadMemory::release(int portId)
{
    assert(isBusy_[portId]);
    isBusy_[portId] = false;
    if (retryReq_[portId]) {
        retryReq_[portId] = false;
        portById(portId).sendRetryReq();
    }
}

void
ScratchpadMemory::dequeue()
{
    assert(!packetQueue.empty());
    DeferredPacket deferred_pkt = packetQueue.front();

    MemoryPort &respPort = portById(deferred_pkt.portId);
    bool sent = respPort.sendTimingResp(deferred_pkt.pkt);
    retryResp_[deferred_pkt.portId] = !sent;

    if (sent) {
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
ScratchpadMemory::recvRespRetry(int portId)
{
    assert(retryResp_[portId]);
    retryResp_[portId] = false;
    dequeue();
}

Port &
ScratchpadMemory::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "port")
        return port;
    else if (if_name == "cpu_port")
        return cpuPort;
    return AbstractMemory::getPort(if_name, idx);
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
                                         ScratchpadMemory& _memory, int _id)
    : ResponsePort(_name), mem(_memory), id_(_id)
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
    return mem.recvTimingReq(pkt, id_);
}

void
ScratchpadMemory::MemoryPort::recvRespRetry()
{
    mem.recvRespRetry(id_);
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
