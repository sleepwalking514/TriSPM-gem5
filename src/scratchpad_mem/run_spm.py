import m5
from m5.objects import *
import argparse

# =========================
# Cache 配置
# =========================
class L1Cache(Cache):
    assoc = 4
    tag_latency = 2
    data_latency = 2
    response_latency = 2
    mshrs = 4
    tgts_per_mshr = 8

class L1ICache(L1Cache):
    size = '32KiB'
    is_read_only = True

class L1DCache(L1Cache):
    size = '32KiB'

class L2Cache(Cache):
    size = '512KiB'
    assoc = 16
    tag_latency = 20
    data_latency = 20
    response_latency = 20
    mshrs = 20
    tgts_per_mshr = 12
    prefetcher = StridePrefetcher(degree=8, latency=1)

class MMUCache(Cache):
    size = '8KiB'
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
    def __init__(self, binary, spm_size, spm_latency, spm_bw):
        super().__init__()

        # 时钟/模式/内存范围
        self.clk_domain = SrcClockDomain(clock='1GHz', voltage_domain=VoltageDomain())
        self.mem_mode = "timing"

        # 物理内存给到 1GiB
        self.mem_ranges = [AddrRange('1GiB')]

        self.cpu = O3CPU()
        self.cpu.createInterruptController()

        # System MemBus
        self.membus = SystemXBar()

        # DMA MMIO
        self._dma_base_addr = 0xF0000000
        self._dma_size = 0x10000

        # SPM 区域
        self._spm_start_addr = 0x40000000
        self._spm_size_val = self._parse_size(spm_size)

        # cache 只覆盖前 512MiB
        self._cacheable_range = AddrRange('512MiB')
        valid_cache_ranges = [self._cacheable_range]

        # DMA buffer 覆盖后 512MiB 空间
        self._dma_buf_size = self._parse_size('512MiB')
        self._dma_buf_base = self._spm_start_addr - self._dma_buf_size

        assert self._spm_size_val <= self._dma_buf_size, \
            f"SPM size ({self._spm_size_val}) must not exceed DMA buffer size ({self._dma_buf_size})"

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
        # SPM + MMIO 总线（非一致性）
        # =========================
        self.spm_bus = NoncoherentXBar(width=64, frontend_latency=1, forward_latency=1, response_latency=1)

        # SPM 实例
        self.spm = ScratchpadMemory(
            range=AddrRange(start=self._spm_start_addr, size=spm_size),
            latency=spm_latency,
            bandwidth=spm_bw
        )
        self.spm.port = self.spm_bus.mem_side_ports

        # =========================
        # CPU D-side 路由：三条路
        #   1) SPM + DMA MMIO -> spm_bus（绕过 cache）
        #   2) DMA_BUF(uncached) -> membus（绕过 cache）
        #   3) 其余 cacheable -> L1D
        # =========================
        self.cpu_d_splitter = NoncoherentXBar(width=64, frontend_latency=1, forward_latency=1, response_latency=1)
        self.cpu.dcache_port = self.cpu_d_splitter.cpu_side_ports

        # Bridge: SPM + DMA MMIO -> spm_bus
        self.spm_bridge = Bridge(
            ranges=[
                AddrRange(start=self._spm_start_addr, size=self._spm_size_val),
                AddrRange(start=self._dma_base_addr, size=self._dma_size),
            ],
            delay='1ns'
        )
        self.spm_bridge.mem_side_port = self.spm_bus.cpu_side_ports

        # Bridge: DMA_BUF -> membus (uncached)
        self.uc_dma_bridge = Bridge(
            ranges=[
                AddrRange(start=self._dma_buf_base, size=self._dma_buf_size),
            ],
            delay='1ns'
        )
        self.uc_dma_bridge.mem_side_port = self.membus.cpu_side_ports

        # splitter 出口：按地址范围路由
        self.cpu_d_splitter.mem_side_ports = [
            self.spm_bridge.cpu_side_port,     # SPM + MMIO
            self.uc_dma_bridge.cpu_side_port,  # DMA buffer (uncached)
            self.l1d.cpu_side                  # cacheable normal DRAM
        ]

        # =========================
        # SpmDmaEngine (replaces CopyEngine)
        # =========================
        self.spm_dma = SpmDmaEngine(
            pio_addr=self._dma_base_addr,
            pio_size=0x20,
            pio_latency='1ns'
        )

        self.spm_dma.pio = self.spm_bus.mem_side_ports

        self.dma_xbar = NoncoherentXBar(width=64, frontend_latency=1, forward_latency=1, response_latency=1)
        self.spm_dma.dma = self.dma_xbar.cpu_side_ports
        self.dma_xbar.mem_side_ports = [
            self.spm_bus.cpu_side_ports,
            self.membus.cpu_side_ports
        ]

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
        self.process.env = [
            f"SPM_SIZE_BYTES={self._parse_size(spm_size)}",
            f"DMA_BUF_BASE=0x{self._dma_buf_base:x}",
            f"DMA_BUF_SIZE={self._dma_buf_size}"
        ]
        self.workload = SEWorkload.init_compatible(binary)
        self.cpu.workload = self.process
        self.cpu.createThreads()

    def map_spm(self):
        """在 instantiate 后调用"""
        # 映射 SPM
        print(f"Mapping SPM: 0x{self._spm_start_addr:x} size: {self._spm_size_val}")
        self.process.map(self._spm_start_addr, self._spm_start_addr, self._spm_size_val)

        # 映射 DMA MMIO
        print(f"Mapping DMA MMIO: 0x{self._dma_base_addr:x} size: {self._dma_size}")
        self.process.map(self._dma_base_addr, self._dma_base_addr, self._dma_size)

        # 映射 DMA BUF（VA=PA）
        print(f"Mapping DMA BUF: 0x{self._dma_buf_base:x} size: {self._dma_buf_size}")
        self.process.map(self._dma_buf_base, self._dma_buf_base, self._dma_buf_size)

    def _parse_size(self, size_str):
        if isinstance(size_str, int):
            return size_str
        units = {'KiB': 1024, 'MiB': 1024**2, 'GiB': 1024**3, 'kB': 1000, 'MB': 1000**2}
        for unit, mul in units.items():
            if size_str.endswith(unit):
                return int(size_str[:-len(unit)]) * mul
        return int(size_str)


if __name__ == "__m5_main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', type=str, required=True, help='Path to binary')
    parser.add_argument('--spm_size', type=str, default='64KiB', help='Size of SPM')
    parser.add_argument('--spm_lat', type=str, default='1ns', help='Latency of SPM')
    parser.add_argument('--spm_bw', type=str, default='100GB/s', help='Bandwidth of SPM')
    args = parser.parse_args()

    root = Root(full_system=False)

    root.system = SPMSystem(
        binary=args.binary,
        spm_size=args.spm_size,
        spm_latency=args.spm_lat,
        spm_bw=args.spm_bw
    )

    print("Instantiating...")
    m5.instantiate()

    print("Mapping spm regions...")
    root.system.map_spm()

    print("Starting simulation...")
    exit_event = m5.simulate()
    print(f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}")
