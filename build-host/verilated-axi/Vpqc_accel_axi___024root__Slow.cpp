// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design implementation internals
// See Vpqc_accel_axi.h for the primary calling header

#include "Vpqc_accel_axi__pch.h"

void Vpqc_accel_axi___024root___ctor_var_reset(Vpqc_accel_axi___024root* vlSelf);

Vpqc_accel_axi___024root::Vpqc_accel_axi___024root(Vpqc_accel_axi__Syms* symsp, const char* namep)
 {
    vlSymsp = symsp;
    vlNamep = strdup(namep);
    // Reset structure values
    Vpqc_accel_axi___024root___ctor_var_reset(this);
}

void Vpqc_accel_axi___024root::__Vconfigure(bool first) {
    (void)first;  // Prevent unused variable warning
}

Vpqc_accel_axi___024root::~Vpqc_accel_axi___024root() {
    VL_DO_DANGLING(std::free(const_cast<char*>(vlNamep)), vlNamep);
}
