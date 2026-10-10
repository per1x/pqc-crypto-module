// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Model implementation (design independent parts)

#include "Vkeccak_f1600__pch.h"

//============================================================
// Constructors

Vkeccak_f1600::Vkeccak_f1600(VerilatedContext* _vcontextp__, const char* _vcname__)
    : VerilatedModel{*_vcontextp__}
    , vlSymsp{new Vkeccak_f1600__Syms(contextp(), _vcname__, this)}
    , clk{vlSymsp->TOP.clk}
    , rst_n{vlSymsp->TOP.rst_n}
    , start{vlSymsp->TOP.start}
    , done{vlSymsp->TOP.done}
    , wr_en{vlSymsp->TOP.wr_en}
    , wr_addr{vlSymsp->TOP.wr_addr}
    , rd_addr{vlSymsp->TOP.rd_addr}
    , wr_data{vlSymsp->TOP.wr_data}
    , rd_data{vlSymsp->TOP.rd_data}
    , rootp{&(vlSymsp->TOP)}
{
    // Register model with the context
    contextp()->addModel(this);
}

Vkeccak_f1600::Vkeccak_f1600(const char* _vcname__)
    : Vkeccak_f1600(Verilated::threadContextp(), _vcname__)
{
}

//============================================================
// Destructor

Vkeccak_f1600::~Vkeccak_f1600() {
    delete vlSymsp;
}

//============================================================
// Evaluation function

#ifdef VL_DEBUG
void Vkeccak_f1600___024root___eval_debug_assertions(Vkeccak_f1600___024root* vlSelf);
#endif  // VL_DEBUG
void Vkeccak_f1600___024root___eval_static(Vkeccak_f1600___024root* vlSelf);
void Vkeccak_f1600___024root___eval_initial(Vkeccak_f1600___024root* vlSelf);
void Vkeccak_f1600___024root___eval_settle(Vkeccak_f1600___024root* vlSelf);
void Vkeccak_f1600___024root___eval(Vkeccak_f1600___024root* vlSelf);

void Vkeccak_f1600::eval_step() {
    VL_DEBUG_IF(VL_DBG_MSGF("+++++TOP Evaluate Vkeccak_f1600::eval_step\n"); );
#ifdef VL_DEBUG
    // Debug assertions
    Vkeccak_f1600___024root___eval_debug_assertions(&(vlSymsp->TOP));
#endif  // VL_DEBUG
    vlSymsp->__Vm_deleter.deleteAll();
    if (VL_UNLIKELY(!vlSymsp->__Vm_didInit)) {
        VL_DEBUG_IF(VL_DBG_MSGF("+ Initial\n"););
        Vkeccak_f1600___024root___eval_static(&(vlSymsp->TOP));
        Vkeccak_f1600___024root___eval_initial(&(vlSymsp->TOP));
        Vkeccak_f1600___024root___eval_settle(&(vlSymsp->TOP));
        vlSymsp->__Vm_didInit = true;
    }
    VL_DEBUG_IF(VL_DBG_MSGF("+ Eval\n"););
    Vkeccak_f1600___024root___eval(&(vlSymsp->TOP));
    // Evaluate cleanup
    Verilated::endOfEval(vlSymsp->__Vm_evalMsgQp);
}

//============================================================
// Events and timing
bool Vkeccak_f1600::eventsPending() { return false; }

uint64_t Vkeccak_f1600::nextTimeSlot() {
    VL_FATAL_MT(__FILE__, __LINE__, "", "No delays in the design");
    return 0;
}

//============================================================
// Utilities

const char* Vkeccak_f1600::name() const {
    return vlSymsp->name();
}

//============================================================
// Invoke final blocks

void Vkeccak_f1600___024root___eval_final(Vkeccak_f1600___024root* vlSelf);

VL_ATTR_COLD void Vkeccak_f1600::final() {
    contextp()->executingFinal(true);
    Vkeccak_f1600___024root___eval_final(&(vlSymsp->TOP));
    contextp()->executingFinal(false);
}

//============================================================
// Implementations of abstract methods from VerilatedModel

const char* Vkeccak_f1600::hierName() const { return vlSymsp->name(); }
const char* Vkeccak_f1600::modelName() const { return "Vkeccak_f1600"; }
unsigned Vkeccak_f1600::threads() const { return 1; }
void Vkeccak_f1600::prepareClone() const { contextp()->prepareClone(); }
void Vkeccak_f1600::atClone() const {
    contextp()->threadPoolpOnClone();
}
