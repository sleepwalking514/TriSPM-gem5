#ifndef __ARCH_RISCV_INSTS_SPM_HH__
#define __ARCH_RISCV_INSTS_SPM_HH__

#include <string>

#include "arch/riscv/insts/static_inst.hh"
#include "arch/riscv/insts/mem.hh"
#include "cpu/exec_context.hh"
#include "cpu/static_inst.hh"

namespace gem5
{

namespace RiscvISA
{

class SpmDma : public RiscvStaticInst
{
  protected:
    using RiscvStaticInst::RiscvStaticInst;

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

class SpmDmaWait : public Load
{
  protected:
    SpmDmaWait(const char *mnem, ExtMachInst _machInst, OpClass __opClass)
        : Load(mnem, _machInst, __opClass)
    {
        memAccessFlags = Request::UNCACHEABLE | Request::STRICT_ORDER;
    }

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_INSTS_SPM_HH__
