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


# =========================
# SPMSystem
# =========================
class SPMSystem(System):
    def __init__(
        self,
        binary,
        enable_spm,
        spm_size,
        spm_latency,
        spm_bw,
        spm_num_banks,
        spm_intlv,
    ):
        super().__init__()

        # 时钟/模式/内存范围
        self.clk_domain = SrcClockDomain(
            clock="1GHz", voltage_domain=VoltageDomain()
        )
        self.mem_mode = "timing"

        # 物理内存给到 1GiB
        self.mem_ranges = [AddrRange("1GiB")]

        self.cpu = O3CPU()
        self.cpu.createInterruptController()

        # System MemBus
        self.membus = SystemXBar()

        if enable_spm:
            # DMA MMIO
            self._dma_base_addr = 0xF0000000
            self._dma_size = 0x10000

            # SPM 区域
            self._spm_start_addr = 0x40000000
            self._spm_size_val = self._parse_size(spm_size)

            # cache 只覆盖前 512MiB
            valid_cache_ranges = [AddrRange("512MiB")]

            # DMA buffer 覆盖后 512MiB 空间
            self._dma_buf_size = self._parse_size("512MiB")
            self._dma_buf_base = self._spm_start_addr - self._dma_buf_size

            assert (
                self._spm_size_val <= self._dma_buf_size
            ), f"SPM size ({self._spm_size_val}) must not exceed DMA buffer size ({self._dma_buf_size})"
        else:
            # cache 覆盖全 1GiB
            valid_cache_ranges = [AddrRange("1GiB")]

        # =========================
        # Cache 层次结构
        # =========================
        self.l2bus = L2XBar()
        self.l2cache = L2Cache(addr_ranges=valid_cache_ranges)

        self.l1i = L1ICache(addr_ranges=valid_cache_ranges)
        self.l1d = L1DCache(addr_ranges=valid_cache_ranges)

        self.iptw_cache = MMUCache(addr_ranges=valid_cache_ranges)
        self.dptw_cache = MMUCache(addr_ranges=valid_cache_ranges)

        # L1 -> L2 bus
        self.l1i.mem_side = self.l2bus.cpu_side_ports
        self.l1d.mem_side = self.l2bus.cpu_side_ports
        self.iptw_cache.mem_side = self.l2bus.cpu_side_ports
        self.dptw_cache.mem_side = self.l2bus.cpu_side_ports

        # L2 bus -> L2 cache -> MemBus
        self.l2bus.mem_side_ports = self.l2cache.cpu_side
        self.l2cache.mem_side = self.membus.cpu_side_ports

        # CPU I / MMU walker
        self.cpu.icache_port = self.l1i.cpu_side
        self.cpu.mmu.itb.walker.port = self.iptw_cache.cpu_side
        self.cpu.mmu.dtb.walker.port = self.dptw_cache.cpu_side

        # =========================
        # CPU dcache → L1D (direct, zero extra latency)
        # =========================
        self.cpu.dcache_port = self.l1d.cpu_side

        # =========================
        # SPM / DMA / uncached bridge — all on L2XBar mem_side
        #
        # L2XBar routes by address:
        #   [0, 512MiB)                  → L2Cache  (cacheable)
        #   [spm_start, +spm_size)       → SPM      (uncacheable)
        #   [dma_buf_base, +512MiB)      → Bridge   (uncacheable → DRAM)
        #   [0xF0000000, +0x40)          → DMA PIO  (uncacheable)
        # =========================
        if enable_spm:
            self.spm = ScratchpadMemory(
                range=AddrRange(start=self._spm_start_addr, size=spm_size),
                latency=spm_latency,
                bandwidth=spm_bw,
                num_banks=spm_num_banks,
                bank_interleave_size=spm_intlv,
            )
            self.spm.port = self.l2bus.mem_side_ports

            self.spm_dma = SpmDmaEngine(
                pio_addr=self._dma_base_addr, pio_size=0x40, pio_latency="1ns"
            )
            self.spm_dma.pio = self.l2bus.mem_side_ports
            self.spm_dma.dma = self.l2bus.cpu_side_ports

            self.uc_bridge = Bridge(
                ranges=[
                    AddrRange(
                        start=self._dma_buf_base, size=self._dma_buf_size
                    )
                ],
                delay="1ns",
                req_size=64,
                resp_size=64,
            )
            self.uc_bridge.mem_side_port = self.membus.cpu_side_ports
            self.l2bus.mem_side_ports = self.uc_bridge.cpu_side_port

        # =========================
        # DRAM Controller
        # =========================
        self.mem_ctrl = MemCtrl()
        self.mem_ctrl.dram = DDR3_1600_8x8()
        self.mem_ctrl.dram.range = self.mem_ranges[0]
        self.mem_ctrl.port = self.membus.mem_side_ports

        self.system_port = self.membus.cpu_side_ports

        # =========================
        # Workload
        # =========================
        self.process = Process(cmd=[binary])
        if enable_spm:
            self.process.env = [
                f"SPM_SIZE_BYTES={self._parse_size(spm_size)}",
                f"DMA_BUF_BASE=0x{self._dma_buf_base:x}",
                f"DMA_BUF_SIZE={self._dma_buf_size}",
            ]
        self.workload = SEWorkload.init_compatible(binary)
        self.cpu.workload = self.process
        self.cpu.createThreads()

    def map_spm(self):
        """在 instantiate 后调用。
        SPM / DMA MMIO / DMA BUF 均标记为 uncacheable，
        使 L1D 收到这些地址的请求时直接 forward 到 L2XBar，不分配 cache line。
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

        print(
            f"Mapping DMA BUF (uncacheable): "
            f"0x{self._dma_buf_base:x} size: {self._dma_buf_size}"
        )
        self.process.map(
            self._dma_buf_base, self._dma_buf_base, self._dma_buf_size, False
        )

    def _parse_size(self, size_str):
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
    parser.add_argument(
        "--binary", type=str, required=True, help="Path to binary"
    )
    parser.add_argument(
        "--cache_baseline",
        action="store_true",
        help="Traditional cache architecture",
    )
    parser.add_argument(
        "--spm_size", type=str, default="256KiB", help="Size of SPM"
    )
    parser.add_argument(
        "--spm_lat", type=str, default="1ns", help="Latency of SPM"
    )
    parser.add_argument(
        "--spm_bw", type=str, default="64GiB/s", help="Bandwidth of SPM"
    )
    parser.add_argument(
        "--spm_num_banks", type=int, default=4, help="Number of SPM banks"
    )
    parser.add_argument(
        "--spm_intlv",
        type=int,
        default=8,
        help="Bank interleave granularity in bytes (power of 2)",
    )
    parser.add_argument(
        "--max-tick",
        type=int,
        default=0,
        help="Stop simulation after this many ticks (0 = unlimited)",
    )
    args = parser.parse_args()

    root = Root(full_system=False)

    root.system = SPMSystem(
        binary=args.binary,
        enable_spm=not args.cache_baseline,
        spm_size=args.spm_size,
        spm_latency=args.spm_lat,
        spm_bw=args.spm_bw,
        spm_num_banks=args.spm_num_banks,
        spm_intlv=args.spm_intlv,
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
