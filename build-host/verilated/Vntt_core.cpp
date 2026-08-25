// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Model implementation (design independent parts)

#include "Vntt_core__pch.h"

//============================================================
// Constructors

Vntt_core::Vntt_core(VerilatedContext* _vcontextp__, const char* _vcname__)
    : VerilatedModel{*_vcontextp__}
    , vlSymsp{new Vntt_core__Syms(contextp(), _vcname__, this)}
    , clk{vlSymsp->TOP.clk}
    , rst_n{vlSymsp->TOP.rst_n}
    , start{vlSymsp->TOP.start}
    , inverse{vlSymsp->TOP.inverse}
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

Vntt_core::Vntt_core(const char* _vcname__)
    : Vntt_core(Verilated::threadContextp(), _vcname__)
{
}

//============================================================
// Destructor

Vntt_core::~Vntt_core() {
    delete vlSymsp;
}

//============================================================
// Evaluation function

#ifdef VL_DEBUG
void Vntt_core___024root___eval_debug_assertions(Vntt_core___024root* vlSelf);
#endif  // VL_DEBUG
void Vntt_core___024root___eval_static(Vntt_core___024root* vlSelf);
void Vntt_core___024root___eval_initial(Vntt_core___024root* vlSelf);
void Vntt_core___024root___eval_settle(Vntt_core___024root* vlSelf);
void Vntt_core___024root___eval(Vntt_core___024root* vlSelf);

void Vntt_core::eval_step() {
    VL_DEBUG_IF(VL_DBG_MSGF("+++++TOP Evaluate Vntt_core::eval_step\n"); );
#ifdef VL_DEBUG
    // Debug assertions
    Vntt_core___024root___eval_debug_assertions(&(vlSymsp->TOP));
#endif  // VL_DEBUG
    vlSymsp->__Vm_deleter.deleteAll();
    if (VL_UNLIKELY(!vlSymsp->__Vm_didInit)) {
        VL_DEBUG_IF(VL_DBG_MSGF("+ Initial\n"););
        Vntt_core___024root___eval_static(&(vlSymsp->TOP));
        Vntt_core___024root___eval_initial(&(vlSymsp->TOP));
        Vntt_core___024root___eval_settle(&(vlSymsp->TOP));
        vlSymsp->__Vm_didInit = true;
    }
    VL_DEBUG_IF(VL_DBG_MSGF("+ Eval\n"););
    Vntt_core___024root___eval(&(vlSymsp->TOP));
    // Evaluate cleanup
    Verilated::endOfEval(vlSymsp->__Vm_evalMsgQp);
}

//============================================================
// Events and timing
bool Vntt_core::eventsPending() { return false; }

uint64_t Vntt_core::nextTimeSlot() {
    VL_FATAL_MT(__FILE__, __LINE__, "", "No delays in the design");
    return 0;
}

//============================================================
// Utilities

const char* Vntt_core::name() const {
    return vlSymsp->name();
}

//============================================================
// Invoke final blocks

void Vntt_core___024root___eval_final(Vntt_core___024root* vlSelf);

VL_ATTR_COLD void Vntt_core::final() {
    contextp()->executingFinal(true);
    Vntt_core___024root___eval_final(&(vlSymsp->TOP));
    contextp()->executingFinal(false);
}

//============================================================
// Implementations of abstract methods from VerilatedModel

const char* Vntt_core::hierName() const { return vlSymsp->name(); }
const char* Vntt_core::modelName() const { return "Vntt_core"; }
unsigned Vntt_core::threads() const { return 1; }
void Vntt_core::prepareClone() const { contextp()->prepareClone(); }
void Vntt_core::atClone() const {
    contextp()->threadPoolpOnClone();
}
