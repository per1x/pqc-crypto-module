// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design implementation internals
// See Vkeccak_f1600.h for the primary calling header

#include "Vkeccak_f1600__pch.h"

void Vkeccak_f1600___024root___ctor_var_reset(Vkeccak_f1600___024root* vlSelf);

Vkeccak_f1600___024root::Vkeccak_f1600___024root(Vkeccak_f1600__Syms* symsp, const char* namep)
 {
    vlSymsp = symsp;
    vlNamep = strdup(namep);
    // Reset structure values
    Vkeccak_f1600___024root___ctor_var_reset(this);
}

void Vkeccak_f1600___024root::__Vconfigure(bool first) {
    (void)first;  // Prevent unused variable warning
}

Vkeccak_f1600___024root::~Vkeccak_f1600___024root() {
    VL_DO_DANGLING(std::free(const_cast<char*>(vlNamep)), vlNamep);
}
