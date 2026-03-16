from m5.params import *
from m5.objects.SimpleMemory import SimpleMemory

class ScratchpadMemory(SimpleMemory):
    type = "ScratchpadMemory"
    cxx_header = "scratchpad_mem/scratchpad_memory.hh"
    cxx_class = "gem5::memory::ScratchpadMemory"