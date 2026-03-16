#include "arch/riscv/insts/spm.hh"

#include <sstream>
#include <string>

#include "arch/riscv/insts/static_inst.hh"
#include "arch/riscv/utility.hh"
#include "cpu/static_inst.hh"

namespace gem5
{

namespace RiscvISA
{

std::string
SpmDma::generateDisassembly(
    Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", "
       << registerName(srcRegIdx(0)) << ", "
       << registerName(srcRegIdx(1));
    return ss.str();
}

std::string
SpmDmaWait::generateDisassembly(
    Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic;
    return ss.str();
}

} // namespace RiscvISA
} // namespace gem5
