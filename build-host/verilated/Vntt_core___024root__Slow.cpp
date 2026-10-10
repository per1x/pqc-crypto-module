// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design implementation internals
// See Vntt_core.h for the primary calling header

#include "Vntt_core__pch.h"

void Vntt_core___024root___ctor_var_reset(Vntt_core___024root* vlSelf);

Vntt_core___024root::Vntt_core___024root(Vntt_core__Syms* symsp, const char* namep)
 {
    vlSymsp = symsp;
    vlNamep = strdup(namep);
    // Reset structure values
    Vntt_core___024root___ctor_var_reset(this);
}

void Vntt_core___024root::__Vconfigure(bool first) {
    (void)first;  // Prevent unused variable warning
}

Vntt_core___024root::~Vntt_core___024root() {
    VL_DO_DANGLING(std::free(const_cast<char*>(vlNamep)), vlNamep);
}
