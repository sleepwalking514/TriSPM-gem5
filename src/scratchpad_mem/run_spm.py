import argparse

import m5
from m5.objects import *


# =========================
# Cache 配置
# =========================
class L1Cache(Cache):
    assoc = 4
    tag_latency = 2
    data_latency = 2
    response_latency = 2
    mshrs = 12
    tgts_per_mshr = 8


class L1ICache(L1Cache):
    size = "32KiB"
    is_read_only = True


class L1DCache(L1Cache):
    size = "32KiB"


class L2Cache(Cache):
    size = "512KiB"
    assoc = 16
    tag_latency = 10
    data_latency = 10
    response_latency = 5
    mshrs = 20
    tgts_per_mshr = 12
    prefetcher = StridePrefetcher(degree=8, latency=1)


class MMUCache(Cache):
    size = "8KiB"
    assoc = 4
    tag_latency = 1
    data_latency = 1
    response_latency = 1
    mshrs = 20
    tgts_per_mshr = 12
    writeback_clean = True


class SPMSystem(System):
    def __init__(
        self,
        binary,
        enable_spm,
        l1d_size,
        l2_size,
        spm_size,
        spm_latency,
        spm_bw,
        spm_single_port,
        spm_num_banks,
        spm_intlv,
        dma_pio_latency,
        dma_desc_latency,
        dram_type,
        system_xbar_width,
        l2_xbar_width,
        dma_max_descriptors=32,
    ):
        super().__init__()

        self.clk_domain = SrcClockDomain(
            clock="1GHz", voltage_domain=VoltageDomain()
        )
        self.mem_mode = "timing"
        self.mem_ranges = [AddrRange("1GiB")]

        self.cpu = O3CPU()
        self.cpu.createInterruptController()

        self.membus = SystemXBar(width=system_xbar_width)
        valid_cache_ranges = [AddrRange("1GiB")]

        if enable_spm:
            self._dma_base_addr = 0xF0000000
            self._dma_size = 0x10000

            self._spm_start_addr = 0x40000000
            self._spm_size_val = self._parse_size(spm_size)

        # ===================== Cache hierarchy =====================
        self.l2bus = L2XBar(width=l2_xbar_width)
        self.l2cache = L2Cache(size=l2_size, addr_ranges=valid_cache_ranges)

        self.l1i = L1ICache(addr_ranges=valid_cache_ranges)
        self.l1d = L1DCache(size=l1d_size, addr_ranges=valid_cache_ranges)

        self.iptw_cache = MMUCache(addr_ranges=valid_cache_ranges)
        self.dptw_cache = MMUCache(addr_ranges=valid_cache_ranges)

        self.l1i.mem_side = self.l2bus.cpu_side_ports
        self.l1d.mem_side = self.l2bus.cpu_side_ports
        self.iptw_cache.mem_side = self.l2bus.cpu_side_ports
        self.dptw_cache.mem_side = self.l2bus.cpu_side_ports

        self.l2bus.mem_side_ports = self.l2cache.cpu_side
        self.l2cache.mem_side = self.membus.cpu_side_ports

        self.cpu.icache_port = self.l1i.cpu_side
        self.cpu.mmu.itb.walker.port = self.iptw_cache.cpu_side
        self.cpu.mmu.dtb.walker.port = self.dptw_cache.cpu_side

        # ===================== CPU data ports =====================
        # dcache_port → L1D  (direct, zero extra latency)
        self.cpu.dcache_port = self.l1d.cpu_side

        if enable_spm:
            # ---------- SPM (tightly-coupled via cpu_port) ----------
            self.spm = ScratchpadMemory(
                range=AddrRange(start=self._spm_start_addr, size=spm_size),
                latency=spm_latency,
                bandwidth=spm_bw,
                single_port=spm_single_port,
                num_banks=spm_num_banks,
                bank_interleave_size=spm_intlv,
            )

            # CPU spm_port → SPM cpu_port (direct, configured SPM latency)
            self.cpu.spm_port = self.spm.cpu_port
            self.cpu.spmAddrStart = self._spm_start_addr
            self.cpu.spmAddrSize = self._spm_size_val

            # SPM bus port → L2XBar  (DMA path)
            self.spm.port = self.l2bus.mem_side_ports

            # ---------- DMA engine ----------
            self.spm_dma = SpmDmaEngine(
                pio_addr=self._dma_base_addr,
                pio_size=0x40,
                pio_latency=dma_pio_latency,
                desc_latency=dma_desc_latency,
                max_descriptors=dma_max_descriptors,
                spm_addr=self._spm_start_addr,
                spm_size=self._spm_size_val,
            )
            self.spm_dma.pio = self.l2bus.mem_side_ports
            self.spm_dma.dma = self.l2bus.cpu_side_ports

        # ===================== DRAM =====================
        self.mem_ctrl = MemCtrl()
        try:
            dram_cls = globals()[dram_type]
        except KeyError as exc:
            raise ValueError(f"Unknown DRAM type: {dram_type}") from exc
        self.mem_ctrl.dram = dram_cls()
        self.mem_ctrl.dram.range = self.mem_ranges[0]
        self.mem_ctrl.port = self.membus.mem_side_ports

        self.system_port = self.membus.cpu_side_ports

        # ===================== Workload =====================
        self.process = Process(cmd=[binary])
        if enable_spm:
            self.process.env = [
                f"SPM_SIZE_BYTES={self._parse_size(spm_size)}",
            ]
        self.workload = SEWorkload.init_compatible(binary)
        self.cpu.workload = self.process
        self.cpu.createThreads()

    def map_spm(self):
        """Post-instantiate: mark SPM and DMA-MMIO as uncacheable.

        SPM is uncacheable so the TLB sets UNCACHEABLE|STRICT_ORDER.
        The LSQ override clears strictlyOrdered for SPM addresses
        and routes them to spm_port, so the O3 pipeline does NOT
        serialise SPM accesses despite the uncacheable flag.
        DMA MMIO remains strictly ordered.  Ordinary DRAM stays cacheable.
        """
        print(
            f"Mapping SPM (uncacheable): "
            f"0x{self._spm_start_addr:x} size: {self._spm_size_val}"
        )
        self.process.map(
            self._spm_start_addr,
            self._spm_start_addr,
            self._spm_size_val,
            False,
        )

        print(
            f"Mapping DMA MMIO (uncacheable): "
            f"0x{self._dma_base_addr:x} size: {self._dma_size}"
        )
        self.process.map(
            self._dma_base_addr, self._dma_base_addr, self._dma_size, False
        )

    @staticmethod
    def _parse_size(size_str):
        if isinstance(size_str, int):
            return size_str
        units = {
            "KiB": 1024,
            "MiB": 1024**2,
            "GiB": 1024**3,
            "kB": 1000,
            "MB": 1000**2,
        }
        for unit, mul in units.items():
            if size_str.endswith(unit):
                return int(size_str[: -len(unit)]) * mul
        return int(size_str)


if __name__ == "__m5_main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=str, required=True)
    parser.add_argument(
        "--cache_baseline",
        action="store_true",
        help="Pure cache architecture (no SPM)",
    )
    parser.add_argument(
        "--l1d_size",
        "--l1d-size",
        dest="l1d_size",
        type=str,
        default="32KiB",
        help="L1 data cache size for both cache-only and SPM systems",
    )
    parser.add_argument(
        "--l2_size",
        "--l2-size",
        dest="l2_size",
        type=str,
        default="512KiB",
        help="L2 cache size for both cache-only and SPM systems",
    )
    parser.add_argument("--spm_size", type=str, default="32KiB")
    parser.add_argument("--spm_lat", type=str, default="2ns")
    parser.add_argument("--spm_bw", type=str, default="32GiB/s")
    parser.add_argument(
        "--spm_single_port",
        action="store_true",
        help="Model CPU and DMA SPM accesses sharing one internal SRAM port",
    )
    parser.add_argument("--spm_num_banks", type=int, default=16)
    parser.add_argument("--spm_intlv", type=int, default=64)
    parser.add_argument("--dma_pio_lat", type=str, default="5ns")
    parser.add_argument("--dma_desc_lat", type=str, default="10ns")
    parser.add_argument("--dram_type", type=str, default="DDR5_6400_4x8")
    parser.add_argument("--system_xbar_width", type=int, default=32)
    parser.add_argument("--l2_xbar_width", type=int, default=32)
    parser.add_argument(
        "--dma_max_descriptors",
        type=int,
        default=32,
        help="Maximum queued DMA descriptors",
    )
    parser.add_argument("--max-tick", type=int, default=0)
    args = parser.parse_args()

    root = Root(full_system=False)
    root.system = SPMSystem(
        binary=args.binary,
        enable_spm=not args.cache_baseline,
        l1d_size=args.l1d_size,
        l2_size=args.l2_size,
        spm_size=args.spm_size,
        spm_latency=args.spm_lat,
        spm_bw=args.spm_bw,
        spm_single_port=args.spm_single_port,
        spm_num_banks=args.spm_num_banks,
        spm_intlv=args.spm_intlv,
        dma_pio_latency=args.dma_pio_lat,
        dma_desc_latency=args.dma_desc_lat,
        dram_type=args.dram_type,
        system_xbar_width=args.system_xbar_width,
        l2_xbar_width=args.l2_xbar_width,
        dma_max_descriptors=args.dma_max_descriptors,
    )

    print("Instantiating...")
    m5.instantiate()

    if not args.cache_baseline:
        print("Mapping spm regions...")
        root.system.map_spm()

    print("Starting simulation...")
    if args.max_tick > 0:
        print(f"  (max tick limit: {args.max_tick})")
        exit_event = m5.simulate(args.max_tick)
    else:
        exit_event = m5.simulate()
    print(f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}")
