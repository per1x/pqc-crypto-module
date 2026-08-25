// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Symbol table internal header
//
// Internal details; most calling programs do not need this header,
// unless using verilator public meta comments.

#ifndef VERILATED_VPQC_ACCEL_AXI__SYMS_H_
#define VERILATED_VPQC_ACCEL_AXI__SYMS_H_  // guard

#include "verilated.h"

// INCLUDE MODEL CLASS

#include "Vpqc_accel_axi.h"

// INCLUDE MODULE CLASSES
#include "Vpqc_accel_axi___024root.h"

// SYMS CLASS (contains all model state)
class alignas(VL_CACHE_LINE_BYTES) Vpqc_accel_axi__Syms final : public VerilatedSyms {
  public:
    // INTERNAL STATE
    Vpqc_accel_axi* const __Vm_modelp;
    VlDeleter __Vm_deleter;
    bool __Vm_didInit = false;

    // MODULE INSTANCE STATE
    Vpqc_accel_axi___024root       TOP;

    // CONSTRUCTORS
    Vpqc_accel_axi__Syms(VerilatedContext* contextp, const char* namep, Vpqc_accel_axi* modelp);
    ~Vpqc_accel_axi__Syms();

    // METHODS
    const char* name() const { return TOP.vlNamep; }
};

#endif  // guard
