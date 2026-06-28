from m5.objects.AbstractMemory import AbstractMemory
from m5.params import *


class ScratchpadMemory(AbstractMemory):
    type = "ScratchpadMemory"
    cxx_header = "scratchpad_mem/scratchpad_memory.hh"
    cxx_class = "gem5::memory::ScratchpadMemory"

    port = ResponsePort("DMA / interconnect side port")
    cpu_port = ResponsePort("CPU direct-access port (tightly-coupled)")

    latency = Param.Latency("1ns", "Per-bank access latency")
    latency_var = Param.Latency("0ns", "Access latency variance")
    bandwidth = Param.MemoryBandwidth(
        "32GiB/s", "Port bandwidth limit (0 = unlimited)"
    )
    single_port = Param.Bool(
        False, "If true, CPU and DMA ports share one internal SRAM port"
    )

    num_banks = Param.Unsigned(16, "Number of SRAM banks")
    bank_interleave_size = Param.Unsigned(
        4, "Bank interleave granularity in bytes (must be power of 2)"
    )
