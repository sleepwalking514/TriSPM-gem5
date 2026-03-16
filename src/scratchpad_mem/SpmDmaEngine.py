from m5.params import *
from m5.proxy import *
from m5.SimObject import SimObject
from m5.objects.ClockedObject import ClockedObject

class SpmDmaEngine(ClockedObject):
    type = 'SpmDmaEngine'
    cxx_header = 'scratchpad_mem/spm_dma_engine.hh'
    cxx_class = 'gem5::SpmDmaEngine'

    system = Param.System(Parent.any, "System this device belongs to")
    pio = ResponsePort("PIO port for MMIO register access")
    dma = RequestPort("DMA port for memory read/write")

    pio_addr = Param.Addr("Base address of PIO registers")
    pio_size = Param.Addr(0x20, "PIO region size (4 regs x 8 bytes)")
    pio_latency = Param.Latency('1ns', 'Latency for PIO register access')
    init_latency = Param.Latency('1ns', 'Startup latency before DMA begins')
