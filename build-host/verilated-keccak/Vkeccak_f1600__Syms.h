// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Symbol table internal header
//
// Internal details; most calling programs do not need this header,
// unless using verilator public meta comments.

#ifndef VERILATED_VKECCAK_F1600__SYMS_H_
#define VERILATED_VKECCAK_F1600__SYMS_H_  // guard

#include "verilated.h"

// INCLUDE MODEL CLASS

#include "Vkeccak_f1600.h"

// INCLUDE MODULE CLASSES
#include "Vkeccak_f1600___024root.h"

// SYMS CLASS (contains all model state)
class alignas(VL_CACHE_LINE_BYTES) Vkeccak_f1600__Syms final : public VerilatedSyms {
  public:
    // INTERNAL STATE
    Vkeccak_f1600* const __Vm_modelp;
    VlDeleter __Vm_deleter;
    bool __Vm_didInit = false;

    // MODULE INSTANCE STATE
    Vkeccak_f1600___024root        TOP;

    // CONSTRUCTORS
    Vkeccak_f1600__Syms(VerilatedContext* contextp, const char* namep, Vkeccak_f1600* modelp);
    ~Vkeccak_f1600__Syms();

    // METHODS
    const char* name() const { return TOP.vlNamep; }
};

#endif  // guard
