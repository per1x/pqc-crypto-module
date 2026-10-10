// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Model implementation (design independent parts)

#include "Vpqc_accel_axi__pch.h"

//============================================================
// Constructors

Vpqc_accel_axi::Vpqc_accel_axi(VerilatedContext* _vcontextp__, const char* _vcname__)
    : VerilatedModel{*_vcontextp__}
    , vlSymsp{new Vpqc_accel_axi__Syms(contextp(), _vcname__, this)}
    , clk{vlSymsp->TOP.clk}
    , rst_n{vlSymsp->TOP.rst_n}
    , s_axi_awaddr{vlSymsp->TOP.s_axi_awaddr}
    , s_axi_awvalid{vlSymsp->TOP.s_axi_awvalid}
    , s_axi_awready{vlSymsp->TOP.s_axi_awready}
    , s_axi_wstrb{vlSymsp->TOP.s_axi_wstrb}
    , s_axi_wvalid{vlSymsp->TOP.s_axi_wvalid}
    , s_axi_wready{vlSymsp->TOP.s_axi_wready}
    , s_axi_bresp{vlSymsp->TOP.s_axi_bresp}
    , s_axi_bvalid{vlSymsp->TOP.s_axi_bvalid}
    , s_axi_bready{vlSymsp->TOP.s_axi_bready}
    , s_axi_araddr{vlSymsp->TOP.s_axi_araddr}
    , s_axi_arvalid{vlSymsp->TOP.s_axi_arvalid}
    , s_axi_arready{vlSymsp->TOP.s_axi_arready}
    , s_axi_rresp{vlSymsp->TOP.s_axi_rresp}
    , s_axi_rvalid{vlSymsp->TOP.s_axi_rvalid}
    , s_axi_rready{vlSymsp->TOP.s_axi_rready}
    , s_axis_tvalid{vlSymsp->TOP.s_axis_tvalid}
    , s_axis_tready{vlSymsp->TOP.s_axis_tready}
    , s_axis_tlast{vlSymsp->TOP.s_axis_tlast}
    , m_axis_tvalid{vlSymsp->TOP.m_axis_tvalid}
    , m_axis_tready{vlSymsp->TOP.m_axis_tready}
    , m_axis_tlast{vlSymsp->TOP.m_axis_tlast}
    , s_axi_wdata{vlSymsp->TOP.s_axi_wdata}
    , s_axi_rdata{vlSymsp->TOP.s_axi_rdata}
    , s_axis_tdata{vlSymsp->TOP.s_axis_tdata}
    , m_axis_tdata{vlSymsp->TOP.m_axis_tdata}
    , rootp{&(vlSymsp->TOP)}
{
    // Register model with the context
    contextp()->addModel(this);
}

Vpqc_accel_axi::Vpqc_accel_axi(const char* _vcname__)
    : Vpqc_accel_axi(Verilated::threadContextp(), _vcname__)
{
}

//============================================================
// Destructor

Vpqc_accel_axi::~Vpqc_accel_axi() {
    delete vlSymsp;
}

//============================================================
// Evaluation function

#ifdef VL_DEBUG
void Vpqc_accel_axi___024root___eval_debug_assertions(Vpqc_accel_axi___024root* vlSelf);
#endif  // VL_DEBUG
void Vpqc_accel_axi___024root___eval_static(Vpqc_accel_axi___024root* vlSelf);
void Vpqc_accel_axi___024root___eval_initial(Vpqc_accel_axi___024root* vlSelf);
void Vpqc_accel_axi___024root___eval_settle(Vpqc_accel_axi___024root* vlSelf);
void Vpqc_accel_axi___024root___eval(Vpqc_accel_axi___024root* vlSelf);

void Vpqc_accel_axi::eval_step() {
    VL_DEBUG_IF(VL_DBG_MSGF("+++++TOP Evaluate Vpqc_accel_axi::eval_step\n"); );
#ifdef VL_DEBUG
    // Debug assertions
    Vpqc_accel_axi___024root___eval_debug_assertions(&(vlSymsp->TOP));
#endif  // VL_DEBUG
    vlSymsp->__Vm_deleter.deleteAll();
    if (VL_UNLIKELY(!vlSymsp->__Vm_didInit)) {
        VL_DEBUG_IF(VL_DBG_MSGF("+ Initial\n"););
        Vpqc_accel_axi___024root___eval_static(&(vlSymsp->TOP));
        Vpqc_accel_axi___024root___eval_initial(&(vlSymsp->TOP));
        Vpqc_accel_axi___024root___eval_settle(&(vlSymsp->TOP));
        vlSymsp->__Vm_didInit = true;
    }
    VL_DEBUG_IF(VL_DBG_MSGF("+ Eval\n"););
    Vpqc_accel_axi___024root___eval(&(vlSymsp->TOP));
    // Evaluate cleanup
    Verilated::endOfEval(vlSymsp->__Vm_evalMsgQp);
}

//============================================================
// Events and timing
bool Vpqc_accel_axi::eventsPending() { return false; }

uint64_t Vpqc_accel_axi::nextTimeSlot() {
    VL_FATAL_MT(__FILE__, __LINE__, "", "No delays in the design");
    return 0;
}

//============================================================
// Utilities

const char* Vpqc_accel_axi::name() const {
    return vlSymsp->name();
}

//============================================================
// Invoke final blocks

void Vpqc_accel_axi___024root___eval_final(Vpqc_accel_axi___024root* vlSelf);

VL_ATTR_COLD void Vpqc_accel_axi::final() {
    contextp()->executingFinal(true);
    Vpqc_accel_axi___024root___eval_final(&(vlSymsp->TOP));
    contextp()->executingFinal(false);
}

//============================================================
// Implementations of abstract methods from VerilatedModel

const char* Vpqc_accel_axi::hierName() const { return vlSymsp->name(); }
const char* Vpqc_accel_axi::modelName() const { return "Vpqc_accel_axi"; }
unsigned Vpqc_accel_axi::threads() const { return 1; }
void Vpqc_accel_axi::prepareClone() const { contextp()->prepareClone(); }
void Vpqc_accel_axi::atClone() const {
    contextp()->threadPoolpOnClone();
}
