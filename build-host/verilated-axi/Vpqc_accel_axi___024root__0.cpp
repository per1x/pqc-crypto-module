// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design implementation internals
// See Vpqc_accel_axi.h for the primary calling header

#include "Vpqc_accel_axi__pch.h"

void Vpqc_accel_axi___024root___eval_triggers_vec__ico(Vpqc_accel_axi___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vpqc_accel_axi___024root___eval_triggers_vec__ico\n"); );
    Vpqc_accel_axi__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    vlSelfRef.__VicoTriggered[0U] = (QData)((IData)(
                                                    (((((((IData)(vlSelfRef.m_axis_tready) 
                                                          != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__m_axis_tready__0)) 
                                                         << 6U) 
                                                        | ((((IData)(vlSelfRef.s_axis_tlast) 
                                                             != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__s_axis_tlast__0)) 
                                                            << 5U) 
                                                           | (((IData)(vlSelfRef.s_axis_tvalid) 
                                                               != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__s_axis_tvalid__0)) 
                                                              << 4U))) 
                                                       | ((((vlSelfRef.s_axis_tdata 
                                                             != vlSelfRef.__Vtrigprevexpr___TOP__s_axis_tdata__0) 
                                                            << 3U) 
                                                           | (((IData)(vlSelfRef.s_axi_rready) 
                                                               != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__s_axi_rready__0)) 
                                                              << 2U)) 
                                                          | ((((IData)(vlSelfRef.s_axi_arvalid) 
                                                               != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__s_axi_arvalid__0)) 
                                                              << 1U) 
                                                             | ((IData)(vlSelfRef.s_axi_araddr) 
                                                                != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__s_axi_araddr__0))))) 
                                                      << 8U) 
                                                     | (((((((IData)(vlSelfRef.s_axi_bready) 
                                                             != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__s_axi_bready__0)) 
                                                            << 3U) 
                                                           | (((IData)(vlSelfRef.s_axi_wvalid) 
                                                               != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__s_axi_wvalid__0)) 
                                                              << 2U)) 
                                                          | ((((IData)(vlSelfRef.s_axi_wstrb) 
                                                               != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__s_axi_wstrb__0)) 
                                                              << 1U) 
                                                             | (vlSelfRef.s_axi_wdata 
                                                                != vlSelfRef.__Vtrigprevexpr___TOP__s_axi_wdata__0))) 
                                                         << 4U) 
                                                        | (((((IData)(vlSelfRef.s_axi_awvalid) 
                                                              != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__s_axi_awvalid__0)) 
                                                             << 3U) 
                                                            | (((IData)(vlSelfRef.s_axi_awaddr) 
                                                                != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__s_axi_awaddr__0)) 
                                                               << 2U)) 
                                                           | ((((IData)(vlSelfRef.rst_n) 
                                                                != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__rst_n__0)) 
                                                               << 1U) 
                                                              | ((IData)(vlSelfRef.clk) 
                                                                 != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__clk__0))))))));
    vlSelfRef.__Vtrigprevexpr___TOP__clk__0 = vlSelfRef.clk;
    vlSelfRef.__Vtrigprevexpr___TOP__rst_n__0 = vlSelfRef.rst_n;
    vlSelfRef.__Vtrigprevexpr___TOP__s_axi_awaddr__0 
        = vlSelfRef.s_axi_awaddr;
    vlSelfRef.__Vtrigprevexpr___TOP__s_axi_awvalid__0 
        = vlSelfRef.s_axi_awvalid;
    vlSelfRef.__Vtrigprevexpr___TOP__s_axi_wdata__0 
        = vlSelfRef.s_axi_wdata;
    vlSelfRef.__Vtrigprevexpr___TOP__s_axi_wstrb__0 
        = vlSelfRef.s_axi_wstrb;
    vlSelfRef.__Vtrigprevexpr___TOP__s_axi_wvalid__0 
        = vlSelfRef.s_axi_wvalid;
    vlSelfRef.__Vtrigprevexpr___TOP__s_axi_bready__0 
        = vlSelfRef.s_axi_bready;
    vlSelfRef.__Vtrigprevexpr___TOP__s_axi_araddr__0 
        = vlSelfRef.s_axi_araddr;
    vlSelfRef.__Vtrigprevexpr___TOP__s_axi_arvalid__0 
        = vlSelfRef.s_axi_arvalid;
    vlSelfRef.__Vtrigprevexpr___TOP__s_axi_rready__0 
        = vlSelfRef.s_axi_rready;
    vlSelfRef.__Vtrigprevexpr___TOP__s_axis_tdata__0 
        = vlSelfRef.s_axis_tdata;
    vlSelfRef.__Vtrigprevexpr___TOP__s_axis_tvalid__0 
        = vlSelfRef.s_axis_tvalid;
    vlSelfRef.__Vtrigprevexpr___TOP__s_axis_tlast__0 
        = vlSelfRef.s_axis_tlast;
    vlSelfRef.__Vtrigprevexpr___TOP__m_axis_tready__0 
        = vlSelfRef.m_axis_tready;
    if (VL_UNLIKELY(((1U & (~ (IData)(vlSelfRef.__VicoDidInit)))))) {
        vlSelfRef.__VicoDidInit = 1U;
        vlSelfRef.__VicoTriggered[0U] = (1ULL | vlSelfRef.__VicoTriggered[0U]);
        vlSelfRef.__VicoTriggered[0U] = (2ULL | vlSelfRef.__VicoTriggered[0U]);
        vlSelfRef.__VicoTriggered[0U] = (4ULL | vlSelfRef.__VicoTriggered[0U]);
        vlSelfRef.__VicoTriggered[0U] = (8ULL | vlSelfRef.__VicoTriggered[0U]);
        vlSelfRef.__VicoTriggered[0U] = (0x0000000000000010ULL 
                                         | vlSelfRef.__VicoTriggered[0U]);
        vlSelfRef.__VicoTriggered[0U] = (0x0000000000000020ULL 
                                         | vlSelfRef.__VicoTriggered[0U]);
        vlSelfRef.__VicoTriggered[0U] = (0x0000000000000040ULL 
                                         | vlSelfRef.__VicoTriggered[0U]);
        vlSelfRef.__VicoTriggered[0U] = (0x0000000000000080ULL 
                                         | vlSelfRef.__VicoTriggered[0U]);
        vlSelfRef.__VicoTriggered[0U] = (0x0000000000000100ULL 
                                         | vlSelfRef.__VicoTriggered[0U]);
        vlSelfRef.__VicoTriggered[0U] = (0x0000000000000200ULL 
                                         | vlSelfRef.__VicoTriggered[0U]);
        vlSelfRef.__VicoTriggered[0U] = (0x0000000000000400ULL 
                                         | vlSelfRef.__VicoTriggered[0U]);
        vlSelfRef.__VicoTriggered[0U] = (0x0000000000000800ULL 
                                         | vlSelfRef.__VicoTriggered[0U]);
        vlSelfRef.__VicoTriggered[0U] = (0x0000000000001000ULL 
                                         | vlSelfRef.__VicoTriggered[0U]);
        vlSelfRef.__VicoTriggered[0U] = (0x0000000000002000ULL 
                                         | vlSelfRef.__VicoTriggered[0U]);
        vlSelfRef.__VicoTriggered[0U] = (0x0000000000004000ULL 
                                         | vlSelfRef.__VicoTriggered[0U]);
    }
}

bool Vpqc_accel_axi___024root___trigger_anySet__ico(const VlUnpacked<QData/*63:0*/, 2> &in) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vpqc_accel_axi___024root___trigger_anySet__ico\n"); );
    // Locals
    IData/*31:0*/ n;
    // Body
    n = 0U;
    do {
        if (in[n]) {
            return (1U);
        }
        n = ((IData)(1U) + n);
    } while ((2U > n));
    return (0U);
}

void Vpqc_accel_axi___024root___eval_ico(Vpqc_accel_axi___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vpqc_accel_axi___024root___eval_ico\n"); );
    Vpqc_accel_axi__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    if ((4ULL & vlSelfRef.__VicoTriggered[0U])) {
        {
            // Inlined CFunc: _ico_sequent__TOP__0
            vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_addr 
                = ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__aw_hold)
                    ? (IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__aw_addr)
                    : (IData)(vlSelfRef.s_axi_awaddr));
        }
    }
    if ((0x0000000000000010ULL & vlSelfRef.__VicoTriggered[0U])) {
        {
            // Inlined CFunc: _ico_sequent__TOP__1
            vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_data 
                = ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__w_hold)
                    ? vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__w_data
                    : vlSelfRef.s_axi_wdata);
        }
    }
    if ((0x0000000000000020ULL & vlSelfRef.__VicoTriggered[0U])) {
        {
            // Inlined CFunc: _ico_sequent__TOP__2
            vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_strb 
                = ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__w_hold)
                    ? (IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__w_strb)
                    : (IData)(vlSelfRef.s_axi_wstrb));
        }
    }
    if ((0x0000000000000800ULL & vlSelfRef.__VicoTriggered[0U])) {
        {
            // Inlined CFunc: _ico_sequent__TOP__3
            vlSelfRef.pqc_accel_axi__DOT__bufa_din = 0U;
            if ((8U & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))) {
                if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__state) 
                              >> 2U)))) {
                    if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__state) 
                                  >> 1U)))) {
                        if ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))) {
                            vlSelfRef.pqc_accel_axi__DOT__bufa_din 
                                = vlSelfRef.pqc_accel_axi__DOT__sq_next;
                        }
                    }
                }
            } else if ((4U & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))) {
                if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__state) 
                              >> 1U)))) {
                    if ((1U & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__state)))) {
                        vlSelfRef.pqc_accel_axi__DOT__bufa_din 
                            = ((IData)(vlSelfRef.pqc_accel_axi__DOT__is_ntt)
                                ? (((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pb_dout) 
                                    << 0x00000010U) 
                                   | (IData)(vlSelfRef.pqc_accel_axi__DOT__lo_lat))
                                : ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__cnt))
                                    ? (IData)((vlSelfRef.pqc_accel_axi__DOT__kec_rd_data 
                                               >> 0x20U))
                                    : (IData)(vlSelfRef.pqc_accel_axi__DOT__kec_rd_data)));
                    }
                }
            } else if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__state) 
                                 >> 1U)))) {
                if ((1U & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__state)))) {
                    vlSelfRef.pqc_accel_axi__DOT__bufa_din 
                        = vlSelfRef.s_axis_tdata;
                }
            }
        }
    }
    if ((0x0000000000001000ULL & vlSelfRef.__VicoTriggered[0U])) {
        {
            // Inlined CFunc: _ico_sequent__TOP__4
            vlSelfRef.pqc_accel_axi__DOT__bufa_we = 0U;
            if ((8U & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))) {
                if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__state) 
                              >> 2U)))) {
                    if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__state) 
                                  >> 1U)))) {
                        if ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))) {
                            vlSelfRef.pqc_accel_axi__DOT__bufa_we 
                                = (((7U == (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state)) 
                                    & ((IData)(vlSelfRef.pqc_accel_axi__DOT__cnt) 
                                       != (IData)(vlSelfRef.pqc_accel_axi__DOT__shk_outlen))) 
                                   & ((3U == (3U & (IData)(vlSelfRef.pqc_accel_axi__DOT__cnt))) 
                                      | ((IData)(vlSelfRef.pqc_accel_axi__DOT__shk_outlen) 
                                         == (0x000003ffU 
                                             & ((IData)(1U) 
                                                + (IData)(vlSelfRef.pqc_accel_axi__DOT__cnt))))));
                        }
                    }
                }
            } else if ((4U & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))) {
                if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__state) 
                              >> 1U)))) {
                    if ((1U & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__state)))) {
                        vlSelfRef.pqc_accel_axi__DOT__bufa_we 
                            = ((1U & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__is_ntt))) 
                               || (1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__cnt)));
                    }
                }
            } else if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__state) 
                                 >> 1U)))) {
                if ((1U & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__state)))) {
                    vlSelfRef.pqc_accel_axi__DOT__bufa_we 
                        = (((IData)(vlSelfRef.s_axis_tvalid) 
                            & (IData)(vlSelfRef.s_axis_tready)) 
                           & (0x0080U > (IData)(vlSelfRef.pqc_accel_axi__DOT__wr_ptr)));
                }
            }
        }
    }
    if ((8ULL & vlSelfRef.__VicoTriggered[0U])) {
        {
            // Inlined CFunc: _ico_sequent__TOP__5
            vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__aw_go 
                = ((IData)(vlSelfRef.s_axi_awready) 
                   & (IData)(vlSelfRef.s_axi_awvalid));
        }
    }
    if ((0x0000000000000040ULL & vlSelfRef.__VicoTriggered[0U])) {
        {
            // Inlined CFunc: _ico_sequent__TOP__6
            vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__w_go 
                = ((IData)(vlSelfRef.s_axi_wready) 
                   & (IData)(vlSelfRef.s_axi_wvalid));
        }
    }
    if ((0x0000000000000048ULL & vlSelfRef.__VicoTriggered[0U])) {
        {
            // Inlined CFunc: _ico_comb__TOP__0
            vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_fire 
                = (((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__aw_hold) 
                    | (IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__aw_go)) 
                   & ((~ (IData)(vlSelfRef.s_axi_bvalid)) 
                      & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__w_hold) 
                         | (IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__w_go))));
        }
    }
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Vpqc_accel_axi___024root___dump_triggers__ico(const VlUnpacked<QData/*63:0*/, 2> &triggers, const std::string &tag);
#endif  // VL_DEBUG

bool Vpqc_accel_axi___024root___eval_phase__ico(Vpqc_accel_axi___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vpqc_accel_axi___024root___eval_phase__ico\n"); );
    Vpqc_accel_axi__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    CData/*0:0*/ __VicoExecute;
    // Body
    Vpqc_accel_axi___024root___eval_triggers_vec__ico(vlSelf);
#ifdef VL_DEBUG
    if (VL_UNLIKELY(vlSymsp->_vm_contextp__->debug())) {
        Vpqc_accel_axi___024root___dump_triggers__ico(vlSelfRef.__VicoTriggered, "ico"s);
    }
#endif
    __VicoExecute = Vpqc_accel_axi___024root___trigger_anySet__ico(vlSelfRef.__VicoTriggered);
    if (__VicoExecute) {
        Vpqc_accel_axi___024root___eval_ico(vlSelf);
    }
    return (__VicoExecute);
}

bool Vpqc_accel_axi___024root___trigger_anySet__act(const VlUnpacked<QData/*63:0*/, 1> &in) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vpqc_accel_axi___024root___trigger_anySet__act\n"); );
    // Locals
    IData/*31:0*/ n;
    // Body
    n = 0U;
    do {
        if (in[n]) {
            return (1U);
        }
        n = ((IData)(1U) + n);
    } while ((1U > n));
    return (0U);
}

void Vpqc_accel_axi___024root___nba_sequent__TOP__1(Vpqc_accel_axi___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vpqc_accel_axi___024root___nba_sequent__TOP__1\n"); );
    Vpqc_accel_axi__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    IData/*31:0*/ __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__4__Vfuncout;
    __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__4__Vfuncout = 0;
    IData/*31:0*/ __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__4__old_val;
    __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__4__old_val = 0;
    IData/*31:0*/ __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__4__new_val;
    __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__4__new_val = 0;
    CData/*3:0*/ __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__4__strb;
    __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__4__strb = 0;
    IData/*31:0*/ __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__5__Vfuncout;
    __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__5__Vfuncout = 0;
    IData/*31:0*/ __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__5__old_val;
    __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__5__old_val = 0;
    IData/*31:0*/ __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__5__new_val;
    __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__5__new_val = 0;
    CData/*3:0*/ __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__5__strb;
    __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__5__strb = 0;
    IData/*31:0*/ __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__6__Vfuncout;
    __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__6__Vfuncout = 0;
    IData/*31:0*/ __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__6__old_val;
    __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__6__old_val = 0;
    IData/*31:0*/ __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__6__new_val;
    __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__6__new_val = 0;
    CData/*3:0*/ __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__6__strb;
    __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__6__strb = 0;
    SData/*8:0*/ __Vdly__pqc_accel_axi__DOT__wr_ptr;
    __Vdly__pqc_accel_axi__DOT__wr_ptr = 0;
    CData/*0:0*/ __Vdly__s_axi_bvalid;
    __Vdly__s_axi_bvalid = 0;
    CData/*0:0*/ __Vdly__pqc_accel_axi__DOT__u_regs__DOT__ar_hold;
    __Vdly__pqc_accel_axi__DOT__u_regs__DOT__ar_hold = 0;
    CData/*0:0*/ __Vdly__s_axi_rvalid;
    __Vdly__s_axi_rvalid = 0;
    // Body
    __Vdly__s_axi_bvalid = vlSelfRef.s_axi_bvalid;
    vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_regs__DOT__reg_in_len 
        = vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_in_len;
    vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_regs__DOT__reg_param 
        = vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_param;
    vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_regs__DOT__reg_mode 
        = vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_mode;
    __Vdly__pqc_accel_axi__DOT__wr_ptr = vlSelfRef.pqc_accel_axi__DOT__wr_ptr;
    vlSelfRef.__Vdly__pqc_accel_axi__DOT__ntt_start 
        = vlSelfRef.pqc_accel_axi__DOT__ntt_start;
    vlSelfRef.__Vdly__pqc_accel_axi__DOT__ntt_inverse 
        = vlSelfRef.pqc_accel_axi__DOT__ntt_inverse;
    vlSelfRef.__Vdly__pqc_accel_axi__DOT__lane_lo = vlSelfRef.pqc_accel_axi__DOT__lane_lo;
    vlSelfRef.__Vdly__pqc_accel_axi__DOT__kec_start 
        = vlSelfRef.pqc_accel_axi__DOT__kec_start;
    vlSelfRef.__Vdly__pqc_accel_axi__DOT__shk_msglen 
        = vlSelfRef.pqc_accel_axi__DOT__shk_msglen;
    vlSelfRef.__Vdly__pqc_accel_axi__DOT__shk_outlen 
        = vlSelfRef.pqc_accel_axi__DOT__shk_outlen;
    vlSelfRef.__Vdly__pqc_accel_axi__DOT__rd_ptr = vlSelfRef.pqc_accel_axi__DOT__rd_ptr;
    vlSelfRef.__Vdly__pqc_accel_axi__DOT__is_ntt = vlSelfRef.pqc_accel_axi__DOT__is_ntt;
    vlSelfRef.__Vdly__pqc_accel_axi__DOT__state = vlSelfRef.pqc_accel_axi__DOT__state;
    vlSelfRef.__Vdly__pqc_accel_axi__DOT__cnt = vlSelfRef.pqc_accel_axi__DOT__cnt;
    __Vdly__pqc_accel_axi__DOT__u_regs__DOT__ar_hold 
        = vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__ar_hold;
    __Vdly__s_axi_rvalid = vlSelfRef.s_axi_rvalid;
    if (vlSelfRef.rst_n) {
        if (vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_fire) {
            __Vdly__s_axi_bvalid = 1U;
        }
        if (((IData)(vlSelfRef.s_axi_bvalid) & (IData)(vlSelfRef.s_axi_bready))) {
            __Vdly__s_axi_bvalid = 0U;
        }
        if (vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__soft_reset_r) {
            __Vdly__pqc_accel_axi__DOT__wr_ptr = 0U;
        } else if (((IData)(vlSelfRef.s_axis_tvalid) 
                    & (IData)(vlSelfRef.s_axis_tready))) {
            __Vdly__pqc_accel_axi__DOT__wr_ptr = ((IData)(vlSelfRef.s_axis_tlast)
                                                   ? 0U
                                                   : 
                                                  (0x000001ffU 
                                                   & ((IData)(1U) 
                                                      + (IData)(vlSelfRef.pqc_accel_axi__DOT__wr_ptr))));
        }
        if (((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__aw_go) 
             & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_fire)))) {
            vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__aw_addr 
                = vlSelfRef.s_axi_awaddr;
            vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__aw_hold = 1U;
        }
        if (((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__w_go) 
             & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_fire)))) {
            vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__w_strb 
                = vlSelfRef.s_axi_wstrb;
            vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__w_data 
                = vlSelfRef.s_axi_wdata;
            vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__w_hold = 1U;
        }
        if (vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_fire) {
            if ((0x00000010U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_addr))) {
                if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_addr) 
                              >> 3U)))) {
                    if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_addr) 
                                  >> 2U)))) {
                        __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__4__strb 
                            = vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_strb;
                        __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__4__new_val 
                            = vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_data;
                        __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__4__old_val 
                            = vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_in_len;
                        __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__4__Vfuncout 
                            = ((((0x0000ff00U & (((8U 
                                                   & (IData)(__Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__4__strb))
                                                   ? 
                                                  (__Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__4__new_val 
                                                   >> 0x18U)
                                                   : 
                                                  (__Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__4__old_val 
                                                   >> 0x18U)) 
                                                 << 8U)) 
                                 | (0x000000ffU & (
                                                   (4U 
                                                    & (IData)(__Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__4__strb))
                                                    ? 
                                                   (__Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__4__new_val 
                                                    >> 0x10U)
                                                    : 
                                                   (__Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__4__old_val 
                                                    >> 0x10U)))) 
                                << 0x00000010U) | (
                                                   (0x0000ff00U 
                                                    & (((2U 
                                                         & (IData)(__Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__4__strb))
                                                         ? 
                                                        (__Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__4__new_val 
                                                         >> 8U)
                                                         : 
                                                        (__Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__4__old_val 
                                                         >> 8U)) 
                                                       << 8U)) 
                                                   | (0x000000ffU 
                                                      & ((1U 
                                                          & (IData)(__Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__4__strb))
                                                          ? __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__4__new_val
                                                          : __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__4__old_val))));
                        vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_regs__DOT__reg_in_len 
                            = __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__4__Vfuncout;
                    }
                }
            }
            if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_addr) 
                          >> 4U)))) {
                if ((8U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_addr))) {
                    if ((4U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_addr))) {
                        __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__5__strb 
                            = vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_strb;
                        __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__5__new_val 
                            = vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_data;
                        __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__5__old_val 
                            = vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_param;
                        __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__5__Vfuncout 
                            = ((((0x0000ff00U & (((8U 
                                                   & (IData)(__Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__5__strb))
                                                   ? 
                                                  (__Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__5__new_val 
                                                   >> 0x18U)
                                                   : 
                                                  (__Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__5__old_val 
                                                   >> 0x18U)) 
                                                 << 8U)) 
                                 | (0x000000ffU & (
                                                   (4U 
                                                    & (IData)(__Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__5__strb))
                                                    ? 
                                                   (__Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__5__new_val 
                                                    >> 0x10U)
                                                    : 
                                                   (__Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__5__old_val 
                                                    >> 0x10U)))) 
                                << 0x00000010U) | (
                                                   (0x0000ff00U 
                                                    & (((2U 
                                                         & (IData)(__Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__5__strb))
                                                         ? 
                                                        (__Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__5__new_val 
                                                         >> 8U)
                                                         : 
                                                        (__Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__5__old_val 
                                                         >> 8U)) 
                                                       << 8U)) 
                                                   | (0x000000ffU 
                                                      & ((1U 
                                                          & (IData)(__Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__5__strb))
                                                          ? __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__5__new_val
                                                          : __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__5__old_val))));
                        vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_regs__DOT__reg_param 
                            = __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__5__Vfuncout;
                    }
                    if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_addr) 
                                  >> 2U)))) {
                        __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__6__strb 
                            = vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_strb;
                        __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__6__new_val 
                            = vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_data;
                        __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__6__old_val 
                            = vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_mode;
                        __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__6__Vfuncout 
                            = ((((0x0000ff00U & (((8U 
                                                   & (IData)(__Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__6__strb))
                                                   ? 
                                                  (__Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__6__new_val 
                                                   >> 0x18U)
                                                   : 
                                                  (__Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__6__old_val 
                                                   >> 0x18U)) 
                                                 << 8U)) 
                                 | (0x000000ffU & (
                                                   (4U 
                                                    & (IData)(__Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__6__strb))
                                                    ? 
                                                   (__Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__6__new_val 
                                                    >> 0x10U)
                                                    : 
                                                   (__Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__6__old_val 
                                                    >> 0x10U)))) 
                                << 0x00000010U) | (
                                                   (0x0000ff00U 
                                                    & (((2U 
                                                         & (IData)(__Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__6__strb))
                                                         ? 
                                                        (__Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__6__new_val 
                                                         >> 8U)
                                                         : 
                                                        (__Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__6__old_val 
                                                         >> 8U)) 
                                                       << 8U)) 
                                                   | (0x000000ffU 
                                                      & ((1U 
                                                          & (IData)(__Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__6__strb))
                                                          ? __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__6__new_val
                                                          : __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__6__old_val))));
                        vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_regs__DOT__reg_mode 
                            = __Vfunc_pqc_accel_axi__DOT__u_regs__DOT__apply_strb__6__Vfuncout;
                    }
                }
            }
            vlSelfRef.s_axi_bresp = 0U;
            vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__aw_hold = 0U;
            vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__w_hold = 0U;
        }
        if (((IData)(vlSelfRef.s_axi_arvalid) & (IData)(vlSelfRef.s_axi_arready))) {
            __Vdly__pqc_accel_axi__DOT__u_regs__DOT__ar_hold = 1U;
            vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__ar_addr 
                = vlSelfRef.s_axi_araddr;
        } else if (((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__ar_hold) 
                    & (~ (IData)(vlSelfRef.s_axi_rvalid)))) {
            vlSelfRef.s_axi_rdata = ((1U == (7U & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__ar_addr) 
                                                   >> 2U)))
                                      ? (((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__st_err) 
                                          << 2U) | 
                                         (((IData)(vlSelfRef.pqc_accel_axi__DOT__busy) 
                                           << 1U) | (IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__st_done)))
                                      : ((2U == (7U 
                                                 & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__ar_addr) 
                                                    >> 2U)))
                                          ? vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_mode
                                          : ((3U == 
                                              (7U & 
                                               ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__ar_addr) 
                                                >> 2U)))
                                              ? vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_param
                                              : ((4U 
                                                  == 
                                                  (7U 
                                                   & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__ar_addr) 
                                                      >> 2U)))
                                                  ? vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_in_len
                                                  : 
                                                 ((5U 
                                                   == 
                                                   (7U 
                                                    & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__ar_addr) 
                                                       >> 2U)))
                                                   ? vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_out_len
                                                   : 
                                                  ((6U 
                                                    == 
                                                    (7U 
                                                     & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__ar_addr) 
                                                        >> 2U)))
                                                    ? vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_errcode
                                                    : 
                                                   (0x00010000U 
                                                    & (- (IData)(
                                                                 (7U 
                                                                  == 
                                                                  (7U 
                                                                   & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__ar_addr) 
                                                                      >> 2U))))))))))));
            vlSelfRef.s_axi_rresp = 0U;
            __Vdly__s_axi_rvalid = 1U;
            __Vdly__pqc_accel_axi__DOT__u_regs__DOT__ar_hold = 0U;
        }
        if (((IData)(vlSelfRef.s_axi_rvalid) & (IData)(vlSelfRef.s_axi_rready))) {
            __Vdly__s_axi_rvalid = 0U;
        }
        if (((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__start_r) 
             | (IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__soft_reset_r))) {
            vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__st_done = 0U;
            vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__st_err = 0U;
        } else if (vlSelfRef.pqc_accel_axi__DOT__done_set) {
            vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__st_done = 1U;
            vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__st_err 
                = vlSelfRef.pqc_accel_axi__DOT__err_set;
        }
        if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__start_r) 
                      | (IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__soft_reset_r))))) {
            if (vlSelfRef.pqc_accel_axi__DOT__out_we) {
                vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_out_len 
                    = vlSelfRef.pqc_accel_axi__DOT__out_len_r;
                vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_errcode 
                    = vlSelfRef.pqc_accel_axi__DOT__errcode_r;
            }
        }
    } else {
        __Vdly__s_axi_bvalid = 0U;
        vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_regs__DOT__reg_in_len = 0U;
        vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_regs__DOT__reg_param = 0U;
        vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_regs__DOT__reg_mode = 0U;
        __Vdly__pqc_accel_axi__DOT__wr_ptr = 0U;
        vlSelfRef.s_axi_bresp = 0U;
        vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__aw_addr = 0U;
        vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__w_strb = 0U;
        vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__w_data = 0U;
        vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__aw_hold = 0U;
        vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__w_hold = 0U;
        __Vdly__pqc_accel_axi__DOT__u_regs__DOT__ar_hold = 0U;
        __Vdly__s_axi_rvalid = 0U;
        vlSelfRef.s_axi_rdata = 0U;
        vlSelfRef.s_axi_rresp = 0U;
        vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__ar_addr = 0U;
        vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__st_done = 0U;
        vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__st_err = 0U;
        vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_out_len = 0U;
        vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_errcode = 0U;
    }
    vlSelfRef.s_axi_bvalid = __Vdly__s_axi_bvalid;
    vlSelfRef.pqc_accel_axi__DOT__wr_ptr = __Vdly__pqc_accel_axi__DOT__wr_ptr;
    vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__ar_hold 
        = __Vdly__pqc_accel_axi__DOT__u_regs__DOT__ar_hold;
    vlSelfRef.s_axi_rvalid = __Vdly__s_axi_rvalid;
    vlSelfRef.s_axi_awready = (1U & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__aw_hold)));
    vlSelfRef.s_axi_wready = (1U & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__w_hold)));
    vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__aw_go 
        = ((IData)(vlSelfRef.s_axi_awready) & (IData)(vlSelfRef.s_axi_awvalid));
    vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__w_go 
        = ((IData)(vlSelfRef.s_axi_wready) & (IData)(vlSelfRef.s_axi_wvalid));
    vlSelfRef.s_axi_arready = (1U & (~ ((IData)(vlSelfRef.s_axi_rvalid) 
                                        | (IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__ar_hold))));
}

void Vpqc_accel_axi___024root___nba_sequent__TOP__2(Vpqc_accel_axi___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vpqc_accel_axi___024root___nba_sequent__TOP__2\n"); );
    Vpqc_accel_axi__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    CData/*1:0*/ __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__ret;
    __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__ret = 0;
    CData/*4:0*/ __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__clr_idx;
    __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__clr_idx = 0;
    CData/*7:0*/ __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__bpos;
    __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__bpos = 0;
    CData/*7:0*/ __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__sq_bpos;
    __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__sq_bpos = 0;
    // Body
    vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__grp 
        = vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__grp;
    vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__len 
        = vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__len;
    vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__state 
        = vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state;
    vlSelfRef.__VdlySet__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v0 = 0U;
    vlSelfRef.__VdlySet__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v1 = 0U;
    vlSelfRef.__VdlySet__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v25 = 0U;
    vlSelfRef.__VdlySet__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v26 = 0U;
    __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__ret = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__ret;
    __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__clr_idx 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__clr_idx;
    __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__bpos = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__bpos;
    __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__sq_bpos 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__sq_bpos;
    vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_sha3__DOT__state 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state;
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__zeroize_d 
        = ((IData)(vlSelfRef.pqc_accel_axi__DOT__core_rst_n) 
           && (IData)(vlSelfRef.pqc_accel_axi__DOT__shk_zeroize));
    if (vlSelfRef.pqc_accel_axi__DOT__core_rst_n) {
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_start_r = 0U;
        if (((IData)(vlSelfRef.pqc_accel_axi__DOT__shk_start) 
             | (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__zeroize_rise))) {
            if (vlSelfRef.pqc_accel_axi__DOT__shk_start) {
                vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__suffix_r 
                    = vlSelfRef.pqc_accel_axi__DOT__shk_suffix;
                __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__ret = 0U;
                vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__rate_r 
                    = vlSelfRef.pqc_accel_axi__DOT__shk_rate;
            } else {
                __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__ret = 2U;
            }
            vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_sha3__DOT__state = 8U;
            __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__clr_idx = 0U;
            __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__bpos = 0U;
            __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__sq_bpos = 0U;
        } else if ((8U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state))) {
            if ((4U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state))) {
                vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_sha3__DOT__state = 0U;
            } else if ((2U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state))) {
                vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_sha3__DOT__state = 0U;
            } else if ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state))) {
                vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_sha3__DOT__state = 0U;
            } else if ((1U & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__perm_busy)))) {
                vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_sha3__DOT__state = 1U;
            }
        } else if ((4U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state))) {
            if ((2U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state))) {
                if ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state))) {
                    if (((9U == (IData)(vlSelfRef.pqc_accel_axi__DOT__state)) 
                         & ((IData)(vlSelfRef.pqc_accel_axi__DOT__cnt) 
                            != (IData)(vlSelfRef.pqc_accel_axi__DOT__shk_outlen)))) {
                        if (((0x000000ffU & ((IData)(1U) 
                                             + (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__sq_bpos))) 
                             == (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__rate_r))) {
                            __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__sq_bpos = 0U;
                            __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__ret = 1U;
                            vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_sha3__DOT__state = 5U;
                        } else {
                            __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__sq_bpos 
                                = (0x000000ffU & ((IData)(1U) 
                                                  + (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__sq_bpos)));
                        }
                    }
                } else if (((IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__perm_busy) 
                            & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_done))) {
                    vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_sha3__DOT__state 
                        = ((1U == (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__ret))
                            ? 7U : 2U);
                }
            } else if ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state))) {
                vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_start_r = 1U;
                vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_sha3__DOT__state = 6U;
            } else {
                __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__bpos = 0U;
                __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__sq_bpos = 0U;
                __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__ret = 1U;
                vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_sha3__DOT__state = 5U;
            }
        } else if ((2U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state))) {
            if ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state))) {
                vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_sha3__DOT__state = 4U;
            } else if (vlSelfRef.pqc_accel_axi__DOT__shk_in_valid) {
                if (((0x000000ffU & ((IData)(1U) + (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__bpos))) 
                     == (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__rate_r))) {
                    __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__bpos = 0U;
                    __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__ret = 0U;
                    vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_sha3__DOT__state = 5U;
                } else {
                    __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__bpos 
                        = (0x000000ffU & ((IData)(1U) 
                                          + (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__bpos)));
                }
            } else if (vlSelfRef.pqc_accel_axi__DOT__shk_flush) {
                vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_sha3__DOT__state = 3U;
            }
        } else if ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state))) {
            if ((0x18U == (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__clr_idx))) {
                __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__clr_idx = 0U;
                vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_sha3__DOT__state 
                    = ((2U == (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__ret))
                        ? 0U : 2U);
            } else {
                __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__clr_idx 
                    = (0x0000001fU & ((IData)(1U) + (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__clr_idx)));
            }
        }
        if (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_start) {
            vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__perm_busy = 1U;
        } else if (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_done) {
            vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__perm_busy = 0U;
        }
    } else {
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__suffix_r = 0x1fU;
        vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_sha3__DOT__state = 0U;
        __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__ret = 0U;
        __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__bpos = 0U;
        __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__sq_bpos = 0U;
        __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__clr_idx = 0U;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_start_r = 0U;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__perm_busy = 0U;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__rate_r = 0x88U;
    }
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__ret 
        = __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__ret;
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__clr_idx 
        = __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__clr_idx;
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__bpos 
        = __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__bpos;
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__sq_bpos 
        = __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__sq_bpos;
}

void Vpqc_accel_axi___024root___nba_sequent__TOP__3(Vpqc_accel_axi___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vpqc_accel_axi___024root___nba_sequent__TOP__3\n"); );
    Vpqc_accel_axi__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    if (vlSelfRef.rst_n) {
        if (vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__soft_reset_r) {
            vlSelfRef.__Vdly__pqc_accel_axi__DOT__state = 0U;
            vlSelfRef.__Vdly__pqc_accel_axi__DOT__cnt = 0U;
            vlSelfRef.pqc_accel_axi__DOT__busy = 0U;
            vlSelfRef.pqc_accel_axi__DOT__done_set = 0U;
            vlSelfRef.pqc_accel_axi__DOT__err_set = 0U;
            vlSelfRef.pqc_accel_axi__DOT__out_we = 0U;
            vlSelfRef.pqc_accel_axi__DOT__out_len_r = 0U;
            vlSelfRef.pqc_accel_axi__DOT__errcode_r = 0U;
            vlSelfRef.pqc_accel_axi__DOT__out_words = 0U;
            vlSelfRef.__Vdly__pqc_accel_axi__DOT__rd_ptr = 0U;
            vlSelfRef.__Vdly__pqc_accel_axi__DOT__is_ntt = 0U;
            vlSelfRef.__Vdly__pqc_accel_axi__DOT__ntt_start = 0U;
            vlSelfRef.__Vdly__pqc_accel_axi__DOT__ntt_inverse = 0U;
            vlSelfRef.pqc_accel_axi__DOT__ntt_wr_en = 0U;
            vlSelfRef.pqc_accel_axi__DOT__ntt_wr_addr = 0U;
            vlSelfRef.pqc_accel_axi__DOT__ntt_wr_data = 0U;
            vlSelfRef.pqc_accel_axi__DOT__ntt_rd_addr = 0U;
            vlSelfRef.__Vdly__pqc_accel_axi__DOT__kec_start = 0U;
            vlSelfRef.pqc_accel_axi__DOT__kec_wr_en = 0U;
            vlSelfRef.pqc_accel_axi__DOT__kec_wr_addr = 0U;
            vlSelfRef.pqc_accel_axi__DOT__kec_wr_data = 0ULL;
            vlSelfRef.pqc_accel_axi__DOT__kec_rd_addr = 0U;
            vlSelfRef.pqc_accel_axi__DOT__lo_lat = 0U;
            vlSelfRef.__Vdly__pqc_accel_axi__DOT__lane_lo = 0U;
            vlSelfRef.pqc_accel_axi__DOT__sq_acc = 0U;
            vlSelfRef.pqc_accel_axi__DOT__shk_start = 0U;
            vlSelfRef.pqc_accel_axi__DOT__shk_flush = 0U;
            vlSelfRef.pqc_accel_axi__DOT__shk_zeroize = 0U;
            vlSelfRef.pqc_accel_axi__DOT__shk_rate = 0x88U;
            vlSelfRef.pqc_accel_axi__DOT__shk_suffix = 0x1fU;
            vlSelfRef.__Vdly__pqc_accel_axi__DOT__shk_msglen = 0U;
            vlSelfRef.__Vdly__pqc_accel_axi__DOT__shk_outlen = 0U;
        } else {
            vlSelfRef.pqc_accel_axi__DOT__done_set = 0U;
            vlSelfRef.pqc_accel_axi__DOT__err_set = 0U;
            vlSelfRef.pqc_accel_axi__DOT__out_we = 0U;
            vlSelfRef.__Vdly__pqc_accel_axi__DOT__ntt_start = 0U;
            vlSelfRef.__Vdly__pqc_accel_axi__DOT__kec_start = 0U;
            vlSelfRef.pqc_accel_axi__DOT__ntt_wr_en = 0U;
            vlSelfRef.pqc_accel_axi__DOT__kec_wr_en = 0U;
            vlSelfRef.pqc_accel_axi__DOT__shk_start = 0U;
            vlSelfRef.pqc_accel_axi__DOT__shk_flush = 0U;
            vlSelfRef.pqc_accel_axi__DOT__shk_zeroize = 0U;
            if (((IData)(vlSelfRef.m_axis_tvalid) & (IData)(vlSelfRef.m_axis_tready))) {
                if (vlSelfRef.m_axis_tlast) {
                    vlSelfRef.__Vdly__pqc_accel_axi__DOT__rd_ptr = 0U;
                    vlSelfRef.pqc_accel_axi__DOT__out_words = 0U;
                } else {
                    vlSelfRef.__Vdly__pqc_accel_axi__DOT__rd_ptr 
                        = (0x000001ffU & ((IData)(1U) 
                                          + (IData)(vlSelfRef.pqc_accel_axi__DOT__rd_ptr)));
                }
            }
            if ((8U & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))) {
                if ((4U & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))) {
                    vlSelfRef.__Vdly__pqc_accel_axi__DOT__state 
                        = ((2U & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))
                            ? 0U : ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))
                                     ? 0U : 1U));
                } else if ((2U & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))) {
                    if ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))) {
                        vlSelfRef.pqc_accel_axi__DOT__ntt_rd_addr = 1U;
                        vlSelfRef.__Vdly__pqc_accel_axi__DOT__state = 4U;
                    } else if ((0U == (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state))) {
                        vlSelfRef.__Vdly__pqc_accel_axi__DOT__state = 5U;
                    }
                } else if ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))) {
                    if (((IData)(vlSelfRef.pqc_accel_axi__DOT__cnt) 
                         == (IData)(vlSelfRef.pqc_accel_axi__DOT__shk_outlen))) {
                        vlSelfRef.pqc_accel_axi__DOT__out_len_r 
                            = vlSelfRef.pqc_accel_axi__DOT__shk_outlen;
                        vlSelfRef.pqc_accel_axi__DOT__shk_zeroize = 1U;
                        vlSelfRef.__Vdly__pqc_accel_axi__DOT__state = 0x0aU;
                        vlSelfRef.pqc_accel_axi__DOT__out_words 
                            = (0x000000ffU & (((IData)(3U) 
                                               + (IData)(vlSelfRef.pqc_accel_axi__DOT__shk_outlen)) 
                                              >> 2U));
                    } else if ((7U == (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state))) {
                        vlSelfRef.__Vdly__pqc_accel_axi__DOT__cnt 
                            = (0x000003ffU & ((IData)(1U) 
                                              + (IData)(vlSelfRef.pqc_accel_axi__DOT__cnt)));
                        vlSelfRef.pqc_accel_axi__DOT__sq_acc 
                            = vlSelfRef.pqc_accel_axi__DOT__sq_next;
                    }
                } else if ((2U == (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state))) {
                    vlSelfRef.pqc_accel_axi__DOT__shk_flush = 1U;
                    vlSelfRef.__Vdly__pqc_accel_axi__DOT__cnt = 0U;
                    vlSelfRef.__Vdly__pqc_accel_axi__DOT__state = 9U;
                }
            } else if ((4U & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))) {
                if ((2U & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))) {
                    if ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))) {
                        if (((IData)(vlSelfRef.pqc_accel_axi__DOT__cnt) 
                             == (IData)(vlSelfRef.pqc_accel_axi__DOT__shk_msglen))) {
                            vlSelfRef.__Vdly__pqc_accel_axi__DOT__state = 8U;
                        } else if ((2U == (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state))) {
                            vlSelfRef.__Vdly__pqc_accel_axi__DOT__cnt 
                                = (0x000003ffU & ((IData)(1U) 
                                                  + (IData)(vlSelfRef.pqc_accel_axi__DOT__cnt)));
                        }
                    } else {
                        vlSelfRef.pqc_accel_axi__DOT__shk_start = 1U;
                        vlSelfRef.__Vdly__pqc_accel_axi__DOT__cnt = 0U;
                        vlSelfRef.__Vdly__pqc_accel_axi__DOT__state = 7U;
                    }
                } else if ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))) {
                    vlSelfRef.pqc_accel_axi__DOT__errcode_r = 0U;
                    vlSelfRef.pqc_accel_axi__DOT__out_we = 1U;
                    vlSelfRef.pqc_accel_axi__DOT__err_set = 0U;
                    vlSelfRef.pqc_accel_axi__DOT__done_set = 1U;
                    vlSelfRef.pqc_accel_axi__DOT__busy = 0U;
                    vlSelfRef.__Vdly__pqc_accel_axi__DOT__state = 0U;
                } else if (vlSelfRef.pqc_accel_axi__DOT__is_ntt) {
                    vlSelfRef.pqc_accel_axi__DOT__ntt_rd_addr 
                        = (0x000000ffU & ((IData)(2U) 
                                          + (IData)(vlSelfRef.pqc_accel_axi__DOT__cnt)));
                    if ((1U & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__cnt)))) {
                        vlSelfRef.pqc_accel_axi__DOT__lo_lat 
                            = vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pb_dout;
                    }
                    if ((0x00ffU == (IData)(vlSelfRef.pqc_accel_axi__DOT__cnt))) {
                        vlSelfRef.pqc_accel_axi__DOT__out_len_r = 0x00000200U;
                        vlSelfRef.pqc_accel_axi__DOT__out_words = 0x0080U;
                        vlSelfRef.__Vdly__pqc_accel_axi__DOT__state = 5U;
                    } else {
                        vlSelfRef.__Vdly__pqc_accel_axi__DOT__cnt 
                            = (0x000003ffU & ((IData)(1U) 
                                              + (IData)(vlSelfRef.pqc_accel_axi__DOT__cnt)));
                    }
                } else {
                    vlSelfRef.pqc_accel_axi__DOT__kec_rd_addr 
                        = (0x0000001fU & (((IData)(vlSelfRef.pqc_accel_axi__DOT__cnt) 
                                           >> 1U) + 
                                          (1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__cnt))));
                    if ((0x0031U == (IData)(vlSelfRef.pqc_accel_axi__DOT__cnt))) {
                        vlSelfRef.pqc_accel_axi__DOT__out_len_r = 0x000000c8U;
                        vlSelfRef.pqc_accel_axi__DOT__out_words = 0x0032U;
                        vlSelfRef.__Vdly__pqc_accel_axi__DOT__state = 5U;
                    } else {
                        vlSelfRef.__Vdly__pqc_accel_axi__DOT__cnt 
                            = (0x000003ffU & ((IData)(1U) 
                                              + (IData)(vlSelfRef.pqc_accel_axi__DOT__cnt)));
                    }
                }
            } else if ((2U & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))) {
                if ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))) {
                    if (((~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__ntt_start) 
                             | (IData)(vlSelfRef.pqc_accel_axi__DOT__kec_start))) 
                         & ((IData)(vlSelfRef.pqc_accel_axi__DOT__is_ntt)
                             ? (IData)(vlSelfRef.pqc_accel_axi__DOT__ntt_done)
                             : (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_done)))) {
                        vlSelfRef.__Vdly__pqc_accel_axi__DOT__cnt = 0U;
                        vlSelfRef.pqc_accel_axi__DOT__ntt_rd_addr = 0U;
                        vlSelfRef.pqc_accel_axi__DOT__kec_rd_addr = 0U;
                        vlSelfRef.__Vdly__pqc_accel_axi__DOT__state 
                            = ((IData)(vlSelfRef.pqc_accel_axi__DOT__is_ntt)
                                ? 0x0bU : 4U);
                    }
                } else {
                    if (vlSelfRef.pqc_accel_axi__DOT__is_ntt) {
                        vlSelfRef.__Vdly__pqc_accel_axi__DOT__ntt_start = 1U;
                    } else {
                        vlSelfRef.__Vdly__pqc_accel_axi__DOT__kec_start = 1U;
                    }
                    vlSelfRef.__Vdly__pqc_accel_axi__DOT__state = 3U;
                }
            } else if ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))) {
                if (vlSelfRef.pqc_accel_axi__DOT__is_ntt) {
                    vlSelfRef.pqc_accel_axi__DOT__ntt_wr_en = 1U;
                    vlSelfRef.pqc_accel_axi__DOT__ntt_wr_addr 
                        = (0x000000ffU & (IData)(vlSelfRef.pqc_accel_axi__DOT__cnt));
                    vlSelfRef.pqc_accel_axi__DOT__ntt_wr_data 
                        = (0x0000ffffU & ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__cnt))
                                           ? (vlSelfRef.pqc_accel_axi__DOT__bufa_dout 
                                              >> 0x00000010U)
                                           : vlSelfRef.pqc_accel_axi__DOT__bufa_dout));
                    if ((0x00ffU == (IData)(vlSelfRef.pqc_accel_axi__DOT__cnt))) {
                        vlSelfRef.__Vdly__pqc_accel_axi__DOT__cnt = 0U;
                        vlSelfRef.__Vdly__pqc_accel_axi__DOT__state = 2U;
                    } else {
                        vlSelfRef.__Vdly__pqc_accel_axi__DOT__cnt 
                            = (0x000003ffU & ((IData)(1U) 
                                              + (IData)(vlSelfRef.pqc_accel_axi__DOT__cnt)));
                    }
                } else {
                    if ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__cnt))) {
                        vlSelfRef.pqc_accel_axi__DOT__kec_wr_en = 1U;
                        vlSelfRef.pqc_accel_axi__DOT__kec_wr_addr 
                            = (0x0000001fU & ((IData)(vlSelfRef.pqc_accel_axi__DOT__cnt) 
                                              >> 1U));
                        vlSelfRef.pqc_accel_axi__DOT__kec_wr_data 
                            = (((QData)((IData)(vlSelfRef.pqc_accel_axi__DOT__bufa_dout)) 
                                << 0x00000020U) | (QData)((IData)(vlSelfRef.pqc_accel_axi__DOT__lane_lo)));
                    } else {
                        vlSelfRef.__Vdly__pqc_accel_axi__DOT__lane_lo 
                            = vlSelfRef.pqc_accel_axi__DOT__bufa_dout;
                    }
                    if ((0x0031U == (IData)(vlSelfRef.pqc_accel_axi__DOT__cnt))) {
                        vlSelfRef.__Vdly__pqc_accel_axi__DOT__cnt = 0U;
                        vlSelfRef.__Vdly__pqc_accel_axi__DOT__state = 2U;
                    } else {
                        vlSelfRef.__Vdly__pqc_accel_axi__DOT__cnt 
                            = (0x000003ffU & ((IData)(1U) 
                                              + (IData)(vlSelfRef.pqc_accel_axi__DOT__cnt)));
                    }
                }
            } else if (vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__start_r) {
                if (((IData)((((0x0000000aU == vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_mode) 
                               & (0U == (0x00000700U 
                                         & vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_param))) 
                              & (((((0U != (0x000000ffU 
                                            & (vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_param 
                                               >> 8U))) 
                                    & (0xc8U >= (0x000000ffU 
                                                 & (vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_param 
                                                    >> 8U)))) 
                                   & (0U != (vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_param 
                                             >> 0x00000010U))) 
                                  & (0x0200U >= (vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_param 
                                                 >> 0x00000010U))) 
                                 & (0x00000200U >= vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_in_len)))) 
                     | (((IData)(vlSelfRef.pqc_accel_axi__DOT__mode_ntt) 
                         & (0x00000200U == vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_in_len)) 
                        | ((9U == vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_mode) 
                           & (0x000000c8U == vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_in_len))))) {
                    vlSelfRef.pqc_accel_axi__DOT__busy = 1U;
                    vlSelfRef.__Vdly__pqc_accel_axi__DOT__is_ntt 
                        = vlSelfRef.pqc_accel_axi__DOT__mode_ntt;
                    vlSelfRef.__Vdly__pqc_accel_axi__DOT__ntt_inverse 
                        = (8U == vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_mode);
                    vlSelfRef.__Vdly__pqc_accel_axi__DOT__cnt = 0U;
                    vlSelfRef.__Vdly__pqc_accel_axi__DOT__rd_ptr = 0U;
                    vlSelfRef.pqc_accel_axi__DOT__out_words = 0U;
                    vlSelfRef.pqc_accel_axi__DOT__shk_rate 
                        = (0x000000ffU & (vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_param 
                                          >> 8U));
                    vlSelfRef.pqc_accel_axi__DOT__shk_suffix 
                        = (0x000000ffU & vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_param);
                    vlSelfRef.__Vdly__pqc_accel_axi__DOT__shk_msglen 
                        = (0x000003ffU & vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_in_len);
                    vlSelfRef.__Vdly__pqc_accel_axi__DOT__shk_outlen 
                        = (0x000003ffU & (vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_param 
                                          >> 0x00000010U));
                    vlSelfRef.__Vdly__pqc_accel_axi__DOT__state 
                        = ((0x0000000aU == vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_mode)
                            ? 6U : 0x0cU);
                } else {
                    vlSelfRef.pqc_accel_axi__DOT__errcode_r = 3U;
                    vlSelfRef.pqc_accel_axi__DOT__out_len_r = 0U;
                    vlSelfRef.pqc_accel_axi__DOT__out_we = 1U;
                    vlSelfRef.pqc_accel_axi__DOT__err_set = 1U;
                    vlSelfRef.pqc_accel_axi__DOT__done_set = 1U;
                }
            }
        }
        vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__start_r = 0U;
        if (vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_fire) {
            if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_addr) 
                          >> 4U)))) {
                if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_addr) 
                              >> 3U)))) {
                    if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_addr) 
                                  >> 2U)))) {
                        vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__start_r 
                            = (1U & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_strb) 
                                     & vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_data));
                    }
                }
            }
        }
    } else {
        vlSelfRef.__Vdly__pqc_accel_axi__DOT__rd_ptr = 0U;
        vlSelfRef.__Vdly__pqc_accel_axi__DOT__ntt_start = 0U;
        vlSelfRef.__Vdly__pqc_accel_axi__DOT__kec_start = 0U;
        vlSelfRef.__Vdly__pqc_accel_axi__DOT__state = 0U;
        vlSelfRef.__Vdly__pqc_accel_axi__DOT__cnt = 0U;
        vlSelfRef.pqc_accel_axi__DOT__busy = 0U;
        vlSelfRef.pqc_accel_axi__DOT__done_set = 0U;
        vlSelfRef.pqc_accel_axi__DOT__err_set = 0U;
        vlSelfRef.pqc_accel_axi__DOT__out_we = 0U;
        vlSelfRef.pqc_accel_axi__DOT__out_len_r = 0U;
        vlSelfRef.pqc_accel_axi__DOT__errcode_r = 0U;
        vlSelfRef.pqc_accel_axi__DOT__out_words = 0U;
        vlSelfRef.__Vdly__pqc_accel_axi__DOT__is_ntt = 0U;
        vlSelfRef.__Vdly__pqc_accel_axi__DOT__ntt_inverse = 0U;
        vlSelfRef.pqc_accel_axi__DOT__ntt_wr_en = 0U;
        vlSelfRef.pqc_accel_axi__DOT__ntt_wr_addr = 0U;
        vlSelfRef.pqc_accel_axi__DOT__ntt_wr_data = 0U;
        vlSelfRef.pqc_accel_axi__DOT__ntt_rd_addr = 0U;
        vlSelfRef.pqc_accel_axi__DOT__kec_wr_en = 0U;
        vlSelfRef.pqc_accel_axi__DOT__kec_wr_addr = 0U;
        vlSelfRef.pqc_accel_axi__DOT__kec_wr_data = 0ULL;
        vlSelfRef.pqc_accel_axi__DOT__kec_rd_addr = 0U;
        vlSelfRef.pqc_accel_axi__DOT__lo_lat = 0U;
        vlSelfRef.__Vdly__pqc_accel_axi__DOT__lane_lo = 0U;
        vlSelfRef.pqc_accel_axi__DOT__sq_acc = 0U;
        vlSelfRef.pqc_accel_axi__DOT__shk_start = 0U;
        vlSelfRef.pqc_accel_axi__DOT__shk_flush = 0U;
        vlSelfRef.pqc_accel_axi__DOT__shk_zeroize = 0U;
        vlSelfRef.pqc_accel_axi__DOT__shk_rate = 0x88U;
        vlSelfRef.pqc_accel_axi__DOT__shk_suffix = 0x1fU;
        vlSelfRef.__Vdly__pqc_accel_axi__DOT__shk_msglen = 0U;
        vlSelfRef.__Vdly__pqc_accel_axi__DOT__shk_outlen = 0U;
        vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__start_r = 0U;
    }
    vlSelfRef.pqc_accel_axi__DOT__lane_lo = vlSelfRef.__Vdly__pqc_accel_axi__DOT__lane_lo;
    vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_param 
        = vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_regs__DOT__reg_param;
    vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_in_len 
        = vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_regs__DOT__reg_in_len;
    vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_mode 
        = vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_regs__DOT__reg_mode;
    vlSelfRef.pqc_accel_axi__DOT__kec_start = vlSelfRef.__Vdly__pqc_accel_axi__DOT__kec_start;
    vlSelfRef.pqc_accel_axi__DOT__shk_msglen = vlSelfRef.__Vdly__pqc_accel_axi__DOT__shk_msglen;
    vlSelfRef.pqc_accel_axi__DOT__shk_outlen = vlSelfRef.__Vdly__pqc_accel_axi__DOT__shk_outlen;
    vlSelfRef.pqc_accel_axi__DOT__rd_ptr = vlSelfRef.__Vdly__pqc_accel_axi__DOT__rd_ptr;
    vlSelfRef.pqc_accel_axi__DOT__is_ntt = vlSelfRef.__Vdly__pqc_accel_axi__DOT__is_ntt;
    vlSelfRef.pqc_accel_axi__DOT__state = vlSelfRef.__Vdly__pqc_accel_axi__DOT__state;
    vlSelfRef.pqc_accel_axi__DOT__cnt = vlSelfRef.__Vdly__pqc_accel_axi__DOT__cnt;
    vlSelfRef.pqc_accel_axi__DOT__mode_ntt = ((7U == vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_mode) 
                                              | (8U 
                                                 == vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_mode));
    vlSelfRef.s_axis_tready = (1U & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__busy)));
    vlSelfRef.m_axis_tvalid = ((0U != (IData)(vlSelfRef.pqc_accel_axi__DOT__out_words)) 
                               & ((IData)(vlSelfRef.pqc_accel_axi__DOT__rd_ptr) 
                                  < (IData)(vlSelfRef.pqc_accel_axi__DOT__out_words)));
    vlSelfRef.pqc_accel_axi__DOT__shk_in_valid = ((7U 
                                                   == (IData)(vlSelfRef.pqc_accel_axi__DOT__state)) 
                                                  & ((IData)(vlSelfRef.pqc_accel_axi__DOT__cnt) 
                                                     != (IData)(vlSelfRef.pqc_accel_axi__DOT__shk_msglen)));
    vlSelfRef.m_axis_tlast = (((IData)(vlSelfRef.pqc_accel_axi__DOT__rd_ptr) 
                               == (0x000001ffU & ((IData)(vlSelfRef.pqc_accel_axi__DOT__out_words) 
                                                  - (IData)(1U)))) 
                              & (IData)(vlSelfRef.m_axis_tvalid));
}

extern const VlWide<64>/*2047:0*/ Vpqc_accel_axi__ConstPool__CONST_hd522b744_0;

void Vpqc_accel_axi___024root___nba_sequent__TOP__5(Vpqc_accel_axi___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vpqc_accel_axi___024root___nba_sequent__TOP__5\n"); );
    Vpqc_accel_axi__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    QData/*63:0*/ pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_3__rc;
    QData/*63:0*/ pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl;
    QData/*63:0*/ pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_0__rotl;
    QData/*63:0*/ __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__0__x;
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__0__x = 0;
    QData/*63:0*/ __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x;
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x = 0;
    CData/*4:0*/ __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rc__3__r;
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rc__3__r = 0;
    QData/*63:0*/ __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v0;
    __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v0 = 0;
    QData/*63:0*/ __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v1;
    __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v1 = 0;
    QData/*63:0*/ __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v2;
    __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v2 = 0;
    QData/*63:0*/ __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v3;
    __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v3 = 0;
    QData/*63:0*/ __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v4;
    __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v4 = 0;
    QData/*63:0*/ __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v5;
    __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v5 = 0;
    QData/*63:0*/ __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v6;
    __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v6 = 0;
    QData/*63:0*/ __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v7;
    __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v7 = 0;
    QData/*63:0*/ __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v8;
    __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v8 = 0;
    QData/*63:0*/ __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v9;
    __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v9 = 0;
    QData/*63:0*/ __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v10;
    __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v10 = 0;
    QData/*63:0*/ __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v11;
    __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v11 = 0;
    QData/*63:0*/ __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v12;
    __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v12 = 0;
    QData/*63:0*/ __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v13;
    __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v13 = 0;
    QData/*63:0*/ __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v14;
    __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v14 = 0;
    QData/*63:0*/ __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v15;
    __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v15 = 0;
    QData/*63:0*/ __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v16;
    __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v16 = 0;
    QData/*63:0*/ __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v17;
    __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v17 = 0;
    QData/*63:0*/ __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v18;
    __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v18 = 0;
    QData/*63:0*/ __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v19;
    __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v19 = 0;
    QData/*63:0*/ __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v20;
    __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v20 = 0;
    QData/*63:0*/ __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v21;
    __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v21 = 0;
    QData/*63:0*/ __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v22;
    __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v22 = 0;
    QData/*63:0*/ __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v23;
    __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v23 = 0;
    QData/*63:0*/ __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v24;
    __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v24 = 0;
    QData/*63:0*/ __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v25;
    __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v25 = 0;
    CData/*4:0*/ __VdlyDim0__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v25;
    __VdlyDim0__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v25 = 0;
    // Body
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state 
        = vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_sha3__DOT__state;
    if (vlSelfRef.pqc_accel_axi__DOT__core_rst_n) {
        if ((4U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state))) {
            if ((2U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state))) {
                if ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state))) {
                    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__scprod_r 
                        = VL_MULS_III(32, (IData)(0x000005a1U), vlSelfRef.__VdfgRegularize_hebeb780c_0_1);
                    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__scbarr_r 
                        = (0x0000ffffU & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_dout) 
                                          - VL_MULS_III(16, (IData)(0x0d01U), 
                                                        (0x0000ffffU 
                                                         & VL_SHIFTRS_III(16,32,32, 
                                                                          ((IData)(0x02000000U) 
                                                                           + 
                                                                           VL_MULS_III(32, (IData)(0x00004ebfU), vlSelfRef.__VdfgRegularize_hebeb780c_0_1)), 0x0000001aU)))));
                    vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__state = 4U;
                } else {
                    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__prod_r 
                        = ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__inv_r)
                            ? VL_MULS_III(32, vlSelfRef.__VdfgRegularize_hebeb780c_0_2, 
                                          (((- (IData)(
                                                       (1U 
                                                        & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__u_bf_gs_h__DOT__diff) 
                                                           >> 0x0000000fU)))) 
                                            << 0x00000010U) 
                                           | (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__u_bf_gs_h__DOT__diff)))
                            : VL_MULS_III(32, vlSelfRef.__VdfgRegularize_hebeb780c_0_2, 
                                          (((- (IData)(
                                                       (1U 
                                                        & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pb_dout) 
                                                           >> 0x0000000fU)))) 
                                            << 0x00000010U) 
                                           | (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pb_dout))));
                    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__bf_a_r 
                        = vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_dout;
                    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__gs_a_r 
                        = (0x0000ffffU & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__u_bf_gs_h__DOT__sum) 
                                          - VL_MULS_III(16, (IData)(0x0d01U), 
                                                        (0x0000ffffU 
                                                         & VL_SHIFTRS_III(16,32,32, 
                                                                          ((IData)(0x02000000U) 
                                                                           + 
                                                                           VL_MULS_III(32, (IData)(0x00004ebfU), 
                                                                                (((- (IData)(
                                                                                (1U 
                                                                                & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__u_bf_gs_h__DOT__sum) 
                                                                                >> 0x0000000fU)))) 
                                                                                << 0x00000010U) 
                                                                                | (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__u_bf_gs_h__DOT__sum)))), 0x0000001aU)))));
                    vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__state = 2U;
                }
            } else if ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state))) {
                vlSelfRef.pqc_accel_axi__DOT__ntt_done = 1U;
                vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__state = 0U;
            } else if ((0x00ffU == (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__scale_i))) {
                vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__state = 5U;
            } else {
                vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__scale_i 
                    = (0x000001ffU & ((IData)(1U) + (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__scale_i)));
                vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__state = 3U;
            }
        } else if ((2U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state))) {
            if ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state))) {
                vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__state = 7U;
            } else {
                vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__state = 1U;
                if ((((IData)(1U) + (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__j)) 
                     < ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__grp) 
                        + (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__len)))) {
                    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__j 
                        = (0x000001ffU & ((IData)(1U) 
                                          + (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__j)));
                } else {
                    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__k 
                        = (0x000000ffU & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__inv_r)
                                           ? ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__k) 
                                              - (IData)(1U))
                                           : ((IData)(1U) 
                                              + (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__k))));
                    if ((0x00000100U > ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__grp) 
                                        + ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__len) 
                                           << 1U)))) {
                        vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__grp 
                            = (0x000001ffU & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__grp) 
                                              + ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__len) 
                                                 << 1U)));
                        vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__j 
                            = (0x000001ffU & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__grp) 
                                              + ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__len) 
                                                 << 1U)));
                    } else if (vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__inv_r) {
                        if ((0x0080U == (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__len))) {
                            vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__scale_i = 0U;
                            vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__state = 3U;
                        } else {
                            vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__len 
                                = (0x000001ffU & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__len) 
                                                  << 1U));
                            vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__grp = 0U;
                            vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__j = 0U;
                        }
                    } else if ((2U == (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__len))) {
                        vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__scale_i = 0U;
                        vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__state = 3U;
                    } else {
                        vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__len 
                            = ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__len) 
                               >> 1U);
                        vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__grp = 0U;
                        vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__j = 0U;
                    }
                }
            }
        } else if ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state))) {
            vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__state = 6U;
        } else if (vlSelfRef.pqc_accel_axi__DOT__ntt_start) {
            if (vlSelfRef.pqc_accel_axi__DOT__ntt_inverse) {
                vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__len = 2U;
                vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__k = 0x7fU;
                vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__grp = 0U;
                vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__j = 0U;
                vlSelfRef.pqc_accel_axi__DOT__ntt_done = 0U;
                vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__inv_r = 1U;
            } else {
                vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__len = 0x0080U;
                vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__k = 1U;
                vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__grp = 0U;
                vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__j = 0U;
                vlSelfRef.pqc_accel_axi__DOT__ntt_done = 0U;
                vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__inv_r = 0U;
            }
            vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__state = 1U;
        }
        if (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__busy) {
            __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v0 
                = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[0U];
            vlSelfRef.__VdlySet__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v0 = 1U;
            if ((0x17U == (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__round_cnt))) {
                vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__busy = 0U;
                vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_done = 1U;
            } else {
                vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__round_cnt 
                    = (0x0000001fU & ((IData)(1U) + (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__round_cnt)));
            }
            __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v1 
                = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[1U];
            vlSelfRef.__VdlySet__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v1 = 1U;
            __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v2 
                = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[2U];
            __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v3 
                = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[3U];
            __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v4 
                = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[4U];
            __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v5 
                = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[5U];
            __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v6 
                = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[6U];
            __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v7 
                = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[7U];
            __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v8 
                = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[8U];
            __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v9 
                = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[9U];
            __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v10 
                = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[10U];
            __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v11 
                = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[11U];
            __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v12 
                = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[12U];
            __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v13 
                = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[13U];
            __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v14 
                = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[14U];
            __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v15 
                = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[15U];
            __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v16 
                = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[16U];
            __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v17 
                = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[17U];
            __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v18 
                = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[18U];
            __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v19 
                = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[19U];
            __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v20 
                = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[20U];
            __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v21 
                = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[21U];
            __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v22 
                = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[22U];
            __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v23 
                = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[23U];
            __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v24 
                = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[24U];
        } else {
            if (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_wr_en) {
                if ((0x18U >= (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_wr_addr))) {
                    __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v25 
                        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_wr_data;
                    __VdlyDim0__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v25 
                        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_wr_addr;
                    vlSelfRef.__VdlySet__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v25 = 1U;
                }
            }
            if (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_start) {
                vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__round_cnt = 0U;
                vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_done = 0U;
                vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__busy = 1U;
            }
        }
    } else {
        vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__len = 0x0080U;
        vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__grp = 0U;
        vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__j = 0U;
        vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__k = 1U;
        vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__scale_i = 0U;
        vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__state = 0U;
        vlSelfRef.pqc_accel_axi__DOT__ntt_done = 0U;
        vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__inv_r = 0U;
        vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__prod_r = 0U;
        vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__bf_a_r = 0U;
        vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__gs_a_r = 0U;
        vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__scprod_r = 0U;
        vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__scbarr_r = 0U;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__round_cnt = 0U;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_done = 0U;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__busy = 0U;
        vlSelfRef.__VdlySet__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v26 = 1U;
    }
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__grp = vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__grp;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__len = vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__len;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state 
        = vlSelfRef.__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__state;
    if (vlSelfRef.__VdlySet__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v0) {
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[0U] 
            = __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v0;
    }
    if (vlSelfRef.__VdlySet__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v1) {
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[1U] 
            = __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v1;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[2U] 
            = __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v2;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[3U] 
            = __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v3;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[4U] 
            = __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v4;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[5U] 
            = __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v5;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[6U] 
            = __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v6;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[7U] 
            = __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v7;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[8U] 
            = __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v8;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[9U] 
            = __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v9;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[10U] 
            = __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v10;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[11U] 
            = __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v11;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[12U] 
            = __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v12;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[13U] 
            = __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v13;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[14U] 
            = __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v14;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[15U] 
            = __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v15;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[16U] 
            = __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v16;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[17U] 
            = __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v17;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[18U] 
            = __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v18;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[19U] 
            = __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v19;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[20U] 
            = __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v20;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[21U] 
            = __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v21;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[22U] 
            = __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v22;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[23U] 
            = __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v23;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[24U] 
            = __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v24;
    }
    if (vlSelfRef.__VdlySet__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v25) {
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[__VdlyDim0__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v25] 
            = __VdlyVal__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v25;
    }
    if (vlSelfRef.__VdlySet__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v26) {
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[0U] = 0ULL;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[1U] = 0ULL;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[2U] = 0ULL;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[3U] = 0ULL;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[4U] = 0ULL;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[5U] = 0ULL;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[6U] = 0ULL;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[7U] = 0ULL;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[8U] = 0ULL;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[9U] = 0ULL;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[10U] = 0ULL;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[11U] = 0ULL;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[12U] = 0ULL;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[13U] = 0ULL;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[14U] = 0ULL;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[15U] = 0ULL;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[16U] = 0ULL;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[17U] = 0ULL;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[18U] = 0ULL;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[19U] = 0ULL;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[20U] = 0ULL;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[21U] = 0ULL;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[22U] = 0ULL;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[23U] = 0ULL;
        vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[24U] = 0ULL;
    }
    vlSelfRef.__VdfgRegularize_hebeb780c_0_2 = (((- (IData)(
                                                            (1U 
                                                             & (vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas
                                                                [
                                                                (0x0000007fU 
                                                                 & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__k))] 
                                                                >> 0x0000000fU)))) 
                                                 << 0x00000010U) 
                                                | vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas
                                                [(0x0000007fU 
                                                  & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__k))]);
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__j_hi 
        = (0x000001ffU & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__j) 
                          + (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__len)));
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__u_bf_ct_t__DOT__t 
        = ((vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__prod_r 
            - VL_MULS_III(32, (IData)(0x00000d01U), 
                          (((- (IData)((1U & (VL_MULS_III(16, (IData)(0xf301U), 
                                                          (0x0000ffffU 
                                                           & vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__prod_r)) 
                                              >> 0x0000000fU)))) 
                            << 0x00000010U) | (0x0000ffffU 
                                               & VL_MULS_III(16, (IData)(0xf301U), 
                                                             (0x0000ffffU 
                                                              & vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__prod_r)))))) 
           >> 0x00000010U);
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pb_we = 0U;
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__C[0U] 
        = ((((vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[0U] 
              ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[5U]) 
             ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[10U]) 
            ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[15U]) 
           ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[20U]);
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__C[1U] 
        = ((((vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[1U] 
              ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[6U]) 
             ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[11U]) 
            ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[16U]) 
           ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[21U]);
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__C[2U] 
        = ((((vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[2U] 
              ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[7U]) 
             ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[12U]) 
            ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[17U]) 
           ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[22U]);
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__C[3U] 
        = ((((vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[3U] 
              ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[8U]) 
             ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[13U]) 
            ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[18U]) 
           ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[23U]);
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__C[4U] 
        = ((((vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[4U] 
              ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[9U]) 
             ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[14U]) 
            ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[19U]) 
           ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[24U]);
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__0__x 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__C[1U];
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_0__rotl 
        = ((__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__0__x 
            << 1U) | (__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__0__x 
                      >> 0x0000003fU));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[0U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__C[4U] 
           ^ pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_0__rotl);
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__0__x 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__C[2U];
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_0__rotl 
        = ((__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__0__x 
            << 1U) | (__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__0__x 
                      >> 0x0000003fU));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[1U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__C[0U] 
           ^ pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_0__rotl);
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__0__x 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__C[3U];
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_0__rotl 
        = ((__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__0__x 
            << 1U) | (__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__0__x 
                      >> 0x0000003fU));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[2U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__C[1U] 
           ^ pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_0__rotl);
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__0__x 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__C[4U];
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_0__rotl 
        = ((__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__0__x 
            << 1U) | (__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__0__x 
                      >> 0x0000003fU));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[3U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__C[2U] 
           ^ pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_0__rotl);
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__0__x 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__C[0U];
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_0__rotl 
        = ((__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__0__x 
            << 1U) | (__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__0__x 
                      >> 0x0000003fU));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[4U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__C[3U] 
           ^ pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_0__rotl);
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[0U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[0U] 
           ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[0U]);
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[5U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[5U] 
           ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[0U]);
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[10U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[10U] 
           ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[0U]);
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[15U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[15U] 
           ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[0U]);
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[20U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[20U] 
           ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[0U]);
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[1U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[1U] 
           ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[1U]);
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[6U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[6U] 
           ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[1U]);
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[11U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[11U] 
           ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[1U]);
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[16U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[16U] 
           ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[1U]);
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[21U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[21U] 
           ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[1U]);
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[2U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[2U] 
           ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[2U]);
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[7U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[7U] 
           ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[2U]);
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[12U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[12U] 
           ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[2U]);
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[17U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[17U] 
           ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[2U]);
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[22U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[22U] 
           ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[2U]);
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[3U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[3U] 
           ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[3U]);
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[8U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[8U] 
           ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[3U]);
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[13U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[13U] 
           ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[3U]);
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[18U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[18U] 
           ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[3U]);
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[23U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[23U] 
           ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[3U]);
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[4U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[4U] 
           ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[4U]);
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[9U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[9U] 
           ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[4U]);
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[14U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[14U] 
           ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[4U]);
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[19U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[19U] 
           ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[4U]);
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[24U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[24U] 
           ^ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[4U]);
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[0U];
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl 
        = __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x;
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[0U] 
        = pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl;
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[5U];
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl 
        = ((__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
            << 0x00000024U) | (__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
                               >> 0x0000001cU));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[16U] 
        = pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl;
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[10U];
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl 
        = ((__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
            << 3U) | (__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
                      >> 0x0000003dU));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[7U] 
        = pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl;
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[15U];
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl 
        = ((__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
            << 0x00000029U) | (__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
                               >> 0x00000017U));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[23U] 
        = pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl;
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[20U];
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl 
        = ((__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
            << 0x00000012U) | (__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
                               >> 0x0000002eU));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[14U] 
        = pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl;
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[1U];
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl 
        = ((__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
            << 1U) | (__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
                      >> 0x0000003fU));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[10U] 
        = pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl;
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[6U];
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl 
        = ((__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
            << 0x0000002cU) | (__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
                               >> 0x00000014U));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[1U] 
        = pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl;
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[11U];
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl 
        = ((__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
            << 0x0000000aU) | (__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
                               >> 0x00000036U));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[17U] 
        = pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl;
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[16U];
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl 
        = ((__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
            << 0x0000002dU) | (__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
                               >> 0x00000013U));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[8U] 
        = pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl;
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[21U];
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl 
        = ((__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
            << 2U) | (__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
                      >> 0x0000003eU));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[24U] 
        = pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl;
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[2U];
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl 
        = ((__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
            << 0x0000003eU) | (__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
                               >> 2U));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[20U] 
        = pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl;
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[7U];
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl 
        = ((__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
            << 6U) | (__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
                      >> 0x0000003aU));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[11U] 
        = pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl;
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[12U];
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl 
        = ((__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
            << 0x0000002bU) | (__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
                               >> 0x00000015U));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[2U] 
        = pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl;
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[17U];
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl 
        = ((__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
            << 0x0000000fU) | (__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
                               >> 0x00000031U));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[18U] 
        = pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl;
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[22U];
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl 
        = ((__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
            << 0x0000003dU) | (__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
                               >> 3U));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[9U] 
        = pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl;
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[3U];
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl 
        = ((__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
            << 0x0000001cU) | (__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
                               >> 0x00000024U));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[5U] 
        = pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl;
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[8U];
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl 
        = ((__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
            << 0x00000037U) | (__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
                               >> 9U));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[21U] 
        = pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl;
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[13U];
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl 
        = ((__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
            << 0x00000019U) | (__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
                               >> 0x00000027U));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[12U] 
        = pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl;
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[18U];
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl 
        = ((__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
            << 0x00000015U) | (__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
                               >> 0x0000002bU));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[3U] 
        = pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl;
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[23U];
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl 
        = ((__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
            << 0x00000038U) | (__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
                               >> 8U));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[19U] 
        = pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl;
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[4U];
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl 
        = ((__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
            << 0x0000001bU) | (__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
                               >> 0x00000025U));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[15U] 
        = pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl;
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[9U];
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl 
        = ((__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
            << 0x00000014U) | (__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
                               >> 0x0000002cU));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[6U] 
        = pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl;
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[14U];
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl 
        = ((__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
            << 0x00000027U) | (__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
                               >> 0x00000019U));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[22U] 
        = pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl;
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[19U];
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl 
        = ((__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
            << 8U) | (__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
                      >> 0x00000038U));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[13U] 
        = pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl;
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[24U];
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl 
        = ((__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
            << 0x0000000eU) | (__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x 
                               >> 0x00000032U));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[4U] 
        = pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl;
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[0U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[0U] 
           ^ ((~ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[1U]) 
              & vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[2U]));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[5U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[5U] 
           ^ ((~ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[6U]) 
              & vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[7U]));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[10U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[10U] 
           ^ ((~ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[11U]) 
              & vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[12U]));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[15U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[15U] 
           ^ ((~ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[16U]) 
              & vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[17U]));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[20U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[20U] 
           ^ ((~ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[21U]) 
              & vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[22U]));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[1U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[1U] 
           ^ ((~ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[2U]) 
              & vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[3U]));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[6U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[6U] 
           ^ ((~ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[7U]) 
              & vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[8U]));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[11U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[11U] 
           ^ ((~ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[12U]) 
              & vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[13U]));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[16U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[16U] 
           ^ ((~ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[17U]) 
              & vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[18U]));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[21U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[21U] 
           ^ ((~ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[22U]) 
              & vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[23U]));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[2U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[2U] 
           ^ ((~ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[3U]) 
              & vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[4U]));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[7U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[7U] 
           ^ ((~ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[8U]) 
              & vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[9U]));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[12U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[12U] 
           ^ ((~ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[13U]) 
              & vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[14U]));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[17U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[17U] 
           ^ ((~ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[18U]) 
              & vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[19U]));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[22U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[22U] 
           ^ ((~ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[23U]) 
              & vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[24U]));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[3U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[3U] 
           ^ ((~ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[4U]) 
              & vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[0U]));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[8U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[8U] 
           ^ ((~ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[9U]) 
              & vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[5U]));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[13U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[13U] 
           ^ ((~ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[14U]) 
              & vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[10U]));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[18U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[18U] 
           ^ ((~ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[19U]) 
              & vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[15U]));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[23U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[23U] 
           ^ ((~ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[24U]) 
              & vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[20U]));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[4U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[4U] 
           ^ ((~ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[0U]) 
              & vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[1U]));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[9U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[9U] 
           ^ ((~ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[5U]) 
              & vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[6U]));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[14U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[14U] 
           ^ ((~ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[10U]) 
              & vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[11U]));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[19U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[19U] 
           ^ ((~ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[15U]) 
              & vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[16U]));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[24U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[24U] 
           ^ ((~ vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[20U]) 
              & vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[21U]));
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rc__3__r 
        = vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__round_cnt;
    pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_3__rc 
        = (((QData)((IData)(Vpqc_accel_axi__ConstPool__CONST_hd522b744_0
                            [(((IData)(0x0000003fU) 
                               + ((IData)(__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rc__3__r) 
                                  << 6U)) >> 5U)])) 
            << 0x00000020U) | (QData)((IData)(Vpqc_accel_axi__ConstPool__CONST_hd522b744_0
                                              [(0x07fffffeU 
                                                & ((IData)(__Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rc__3__r) 
                                                   << 1U))])));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[0U] 
        = (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[0U] 
           ^ pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_3__rc);
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pb_din = 0U;
    if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state) 
                  >> 2U)))) {
        if ((2U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state))) {
            if ((1U & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state)))) {
                vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pb_we = 1U;
                vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pb_din 
                    = (0x0000ffffU & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__inv_r)
                                       ? (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__u_bf_ct_t__DOT__t)
                                       : ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__bf_a_r) 
                                          - (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__u_bf_ct_t__DOT__t))));
            }
        }
    }
}

void Vpqc_accel_axi___024root___nba_comb__TOP__0(Vpqc_accel_axi___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vpqc_accel_axi___024root___nba_comb__TOP__0\n"); );
    Vpqc_accel_axi__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    CData/*4:0*/ pqc_accel_axi__DOT__u_sha3__DOT__kec_rd_addr;
    pqc_accel_axi__DOT__u_sha3__DOT__kec_rd_addr = 0;
    QData/*63:0*/ pqc_accel_axi__DOT__u_sha3__DOT__kec_rd_data;
    pqc_accel_axi__DOT__u_sha3__DOT__kec_rd_data = 0;
    // Body
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__zeroize_rise 
        = ((~ (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__zeroize_d)) 
           & (IData)(vlSelfRef.pqc_accel_axi__DOT__shk_zeroize));
    pqc_accel_axi__DOT__u_sha3__DOT__kec_rd_addr = 
        (0x0000001fU & ((7U == (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state))
                         ? ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__sq_bpos) 
                            >> 3U) : ((4U == (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state))
                                       ? (((IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__rate_r) 
                                           >> 3U) - (IData)(1U))
                                       : ((0U == (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state))
                                           ? (IData)(vlSelfRef.pqc_accel_axi__DOT__kec_rd_addr)
                                           : ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__bpos) 
                                              >> 3U)))));
    vlSelfRef.pqc_accel_axi__DOT__bufa_addr = 0U;
    vlSelfRef.pqc_accel_axi__DOT__bufa_we = 0U;
    if ((8U & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))) {
        if ((4U & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))) {
            if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__state) 
                          >> 1U)))) {
                if ((1U & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__state)))) {
                    vlSelfRef.pqc_accel_axi__DOT__bufa_addr = 0U;
                }
            }
        } else if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__state) 
                             >> 1U)))) {
            if ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))) {
                vlSelfRef.pqc_accel_axi__DOT__bufa_addr 
                    = (0x0000007fU & ((IData)(vlSelfRef.pqc_accel_axi__DOT__cnt) 
                                      >> 2U));
            }
        }
        if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__state) 
                      >> 2U)))) {
            if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__state) 
                          >> 1U)))) {
                if ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))) {
                    vlSelfRef.pqc_accel_axi__DOT__bufa_we 
                        = (((7U == (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state)) 
                            & ((IData)(vlSelfRef.pqc_accel_axi__DOT__cnt) 
                               != (IData)(vlSelfRef.pqc_accel_axi__DOT__shk_outlen))) 
                           & ((3U == (3U & (IData)(vlSelfRef.pqc_accel_axi__DOT__cnt))) 
                              | ((IData)(vlSelfRef.pqc_accel_axi__DOT__shk_outlen) 
                                 == (0x000003ffU & 
                                     ((IData)(1U) + (IData)(vlSelfRef.pqc_accel_axi__DOT__cnt))))));
                }
            }
        }
    } else if ((4U & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))) {
        if ((2U & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))) {
            vlSelfRef.pqc_accel_axi__DOT__bufa_addr 
                = ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))
                    ? (0x0000007fU & ((2U == (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state))
                                       ? (0x0000007fU 
                                          & (((IData)(1U) 
                                              + (IData)(vlSelfRef.pqc_accel_axi__DOT__cnt)) 
                                             >> 2U))
                                       : ((IData)(vlSelfRef.pqc_accel_axi__DOT__cnt) 
                                          >> 2U))) : 0U);
        } else if ((1U & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__state)))) {
            vlSelfRef.pqc_accel_axi__DOT__bufa_addr 
                = (0x0000007fU & ((IData)(vlSelfRef.pqc_accel_axi__DOT__is_ntt)
                                   ? ((IData)(vlSelfRef.pqc_accel_axi__DOT__cnt) 
                                      >> 1U) : (IData)(vlSelfRef.pqc_accel_axi__DOT__cnt)));
        }
        if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__state) 
                      >> 1U)))) {
            if ((1U & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__state)))) {
                vlSelfRef.pqc_accel_axi__DOT__bufa_we 
                    = ((1U & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__is_ntt))) 
                       || (1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__cnt)));
            }
        }
    } else if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__state) 
                         >> 1U)))) {
        vlSelfRef.pqc_accel_axi__DOT__bufa_addr = (0x0000007fU 
                                                   & ((1U 
                                                       & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))
                                                       ? 
                                                      ((IData)(vlSelfRef.pqc_accel_axi__DOT__is_ntt)
                                                        ? 
                                                       (0x0000007fU 
                                                        & (((IData)(1U) 
                                                            + (IData)(vlSelfRef.pqc_accel_axi__DOT__cnt)) 
                                                           >> 1U))
                                                        : 
                                                       ((IData)(1U) 
                                                        + (IData)(vlSelfRef.pqc_accel_axi__DOT__cnt)))
                                                       : (IData)(vlSelfRef.pqc_accel_axi__DOT__wr_ptr)));
        if ((1U & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__state)))) {
            vlSelfRef.pqc_accel_axi__DOT__bufa_we = 
                (((IData)(vlSelfRef.s_axis_tvalid) 
                  & (IData)(vlSelfRef.s_axis_tready)) 
                 & (0x0080U > (IData)(vlSelfRef.pqc_accel_axi__DOT__wr_ptr)));
        }
    }
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_wr_en = 0U;
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_start 
        = ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_start_r) 
           | ((IData)(vlSelfRef.pqc_accel_axi__DOT__kec_start) 
              & (0U == (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state))));
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_we = 0U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_din = 0U;
    if ((4U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state))) {
        if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state) 
                      >> 1U)))) {
            if ((1U & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state)))) {
                vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_we = 1U;
                vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_din 
                    = (0x0000ffffU & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__inv_r)
                                       ? ((vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__scprod_r 
                                           - VL_MULS_III(32, (IData)(0x00000d01U), 
                                                         (((- (IData)(
                                                                      (1U 
                                                                       & (VL_MULS_III(16, (IData)(0xf301U), 
                                                                                (0x0000ffffU 
                                                                                & vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__scprod_r)) 
                                                                          >> 0x0000000fU)))) 
                                                           << 0x00000010U) 
                                                          | (0x0000ffffU 
                                                             & VL_MULS_III(16, (IData)(0xf301U), 
                                                                           (0x0000ffffU 
                                                                            & vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__scprod_r)))))) 
                                          >> 0x00000010U)
                                       : (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__scbarr_r)));
            }
        }
    } else if ((2U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state))) {
        if ((1U & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state)))) {
            vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_we = 1U;
            vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_din 
                = (0x0000ffffU & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__inv_r)
                                   ? (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__gs_a_r)
                                   : ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__bf_a_r) 
                                      + (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__u_bf_ct_t__DOT__t))));
        }
    } else if ((1U & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state)))) {
        vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_we 
            = vlSelfRef.pqc_accel_axi__DOT__ntt_wr_en;
        vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_din 
            = vlSelfRef.pqc_accel_axi__DOT__ntt_wr_data;
    }
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_wr_addr 
        = pqc_accel_axi__DOT__u_sha3__DOT__kec_rd_addr;
    pqc_accel_axi__DOT__u_sha3__DOT__kec_rd_data = 
        (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A
         [pqc_accel_axi__DOT__u_sha3__DOT__kec_rd_addr] 
         & (- (QData)((IData)((0x18U >= (IData)(pqc_accel_axi__DOT__u_sha3__DOT__kec_rd_addr))))));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_wr_data = 0ULL;
    if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state) 
                  >> 3U)))) {
        if ((4U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state))) {
            if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state) 
                          >> 1U)))) {
                if ((1U & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state)))) {
                    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_wr_en = 1U;
                    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_wr_data 
                        = (0x8000000000000000ULL ^ pqc_accel_axi__DOT__u_sha3__DOT__kec_rd_data);
                }
            }
        } else if ((2U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state))) {
            vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_wr_en 
                = ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state)) 
                   || (IData)(vlSelfRef.pqc_accel_axi__DOT__shk_in_valid));
            vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_wr_data 
                = ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state))
                    ? (pqc_accel_axi__DOT__u_sha3__DOT__kec_rd_data 
                       ^ ((QData)((IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__suffix_r)) 
                          << (0x00000038U & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__bpos) 
                                             << 3U))))
                    : (pqc_accel_axi__DOT__u_sha3__DOT__kec_rd_data 
                       ^ ((QData)((IData)((0x000000ffU 
                                           & (vlSelfRef.pqc_accel_axi__DOT__bufa_dout 
                                              >> (0x00000018U 
                                                  & ((IData)(vlSelfRef.pqc_accel_axi__DOT__cnt) 
                                                     << 3U)))))) 
                          << (0x00000038U & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__bpos) 
                                             << 3U)))));
        } else {
            vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_wr_en 
                = ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state)) 
                   || (IData)(vlSelfRef.pqc_accel_axi__DOT__kec_wr_en));
            vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_wr_data 
                = ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state))
                    ? 0ULL : vlSelfRef.pqc_accel_axi__DOT__kec_wr_data);
        }
        if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state) 
                      >> 2U)))) {
            if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state) 
                          >> 1U)))) {
                vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_wr_addr 
                    = ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state))
                        ? (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__clr_idx)
                        : (IData)(vlSelfRef.pqc_accel_axi__DOT__kec_wr_addr));
            }
        }
    }
    vlSelfRef.pqc_accel_axi__DOT__kec_rd_data = ((- (QData)((IData)(
                                                                    (0U 
                                                                     == (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state))))) 
                                                 & pqc_accel_axi__DOT__u_sha3__DOT__kec_rd_data);
    vlSelfRef.pqc_accel_axi__DOT__sq_next = (((0x000000ffU 
                                               & (IData)(
                                                         (pqc_accel_axi__DOT__u_sha3__DOT__kec_rd_data 
                                                          >> 
                                                          (0x00000038U 
                                                           & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__sq_bpos) 
                                                              << 3U))))) 
                                              << (0x00000018U 
                                                  & ((IData)(vlSelfRef.pqc_accel_axi__DOT__cnt) 
                                                     << 3U))) 
                                             | (vlSelfRef.pqc_accel_axi__DOT__sq_acc 
                                                & (- (IData)(
                                                             (0U 
                                                              != 
                                                              (3U 
                                                               & (IData)(vlSelfRef.pqc_accel_axi__DOT__cnt)))))));
}

void Vpqc_accel_axi___024root___eval_nba(Vpqc_accel_axi___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vpqc_accel_axi___024root___eval_nba\n"); );
    Vpqc_accel_axi__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    if ((1ULL & vlSelfRef.__VnbaTriggered[0U])) {
        {
            // Inlined CFunc: _nba_sequent__TOP__0
            if (VL_UNLIKELY(((((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_we) 
                               & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pb_we)) 
                              & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_addr) 
                                 == (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pb_addr)))))) {
                VL_WRITEF_NX("[%0t] ram_dp \346\226\255\350\250\200\345\244\261\350\264\245\357\274\232\344\270\244\344\270\252\345\217\243\345\220\214\346\227\266\345\206\231\345\220\214\344\270\200\345\234\260\345\235\200 %0d\n",3, 'T',-12
                             , '#',64,VL_TIME_UNITED_Q(1)
                             , '#',8,(IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_addr));
                VL_STOP_MT("/Volumes/Work/projects/code/pqc-hsm-fpga/hardware/rtl/common/ram_dp.v", 68, "");
            }
            vlSelfRef.__VdlySet__pqc_accel_axi__DOT__u_buf__DOT__mem__v0 = 0U;
            vlSelfRef.__VdlySet__pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem__v1 = 0U;
            vlSelfRef.__VdlySet__pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem__v0 = 0U;
            if (((IData)(vlSelfRef.pqc_accel_axi__DOT__core_rst_n) 
                 & (IData)(vlSelfRef.pqc_accel_axi__DOT__shk_start))) {
                if (VL_UNLIKELY(((((0U != (7U & (IData)(vlSelfRef.pqc_accel_axi__DOT__shk_rate))) 
                                   | (0U == (IData)(vlSelfRef.pqc_accel_axi__DOT__shk_rate))) 
                                  | (0xc8U < (IData)(vlSelfRef.pqc_accel_axi__DOT__shk_rate)))))) {
                    VL_WRITEF_NX("[sha3_core] \351\235\236\346\263\225 rate=%0d\357\274\232\345\277\205\351\241\273\346\230\257 8 \347\232\204\345\200\215\346\225\260\344\270\224\345\234\250 1..200\n",1
                                 , '#',8,vlSelfRef.pqc_accel_axi__DOT__shk_rate);
                    VL_FINISH_MT("/Volumes/Work/projects/code/pqc-hsm-fpga/hardware/rtl/keccak/sha3_core.v", 358, "");
                }
            }
            if (VL_UNLIKELY(((((IData)(vlSelfRef.pqc_accel_axi__DOT__core_rst_n) 
                               & (IData)(vlSelfRef.pqc_accel_axi__DOT__shk_in_valid)) 
                              & (IData)(vlSelfRef.pqc_accel_axi__DOT__shk_flush))))) {
                VL_WRITEF_NX("[sha3_core] in_valid \344\270\216 in_flush \344\270\215\345\276\227\345\220\214\346\227\266\346\213\211\351\253\230\n",0);
                VL_FINISH_MT("/Volumes/Work/projects/code/pqc-hsm-fpga/hardware/rtl/keccak/sha3_core.v", 363, "");
            }
            if (VL_UNLIKELY(((((IData)(vlSelfRef.pqc_accel_axi__DOT__core_rst_n) 
                               & (0U != (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state))) 
                              & ((IData)(vlSelfRef.pqc_accel_axi__DOT__kec_wr_en) 
                                 | (IData)(vlSelfRef.pqc_accel_axi__DOT__kec_start)))))) {
                VL_WRITEF_NX("[sha3_core] \346\265\267\347\273\265\350\277\220\350\241\214\344\270\255\357\274\210state=%0d\357\274\211\345\212\250\347\224\250\344\272\206\347\233\264\351\200\232\345\217\243\357\274\232\345\206\231\345\205\245\344\274\232\350\242\253\345\277\275\347\225\245\n",1
                             , '#',4,vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state);
                VL_FINISH_MT("/Volumes/Work/projects/code/pqc-hsm-fpga/hardware/rtl/keccak/sha3_core.v", 371, "");
            }
            if (vlSelfRef.pqc_accel_axi__DOT__bufa_we) {
                vlSelfRef.__VdlyVal__pqc_accel_axi__DOT__u_buf__DOT__mem__v0 
                    = vlSelfRef.pqc_accel_axi__DOT__bufa_din;
                vlSelfRef.__VdlyDim0__pqc_accel_axi__DOT__u_buf__DOT__mem__v0 
                    = vlSelfRef.pqc_accel_axi__DOT__bufa_addr;
                vlSelfRef.__VdlySet__pqc_accel_axi__DOT__u_buf__DOT__mem__v0 = 1U;
            }
            if (vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pb_we) {
                vlSelfRef.__VdlyVal__pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem__v1 
                    = vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pb_din;
                vlSelfRef.__VdlyDim0__pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem__v1 
                    = vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pb_addr;
                vlSelfRef.__VdlySet__pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem__v1 = 1U;
            }
            if (vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_we) {
                vlSelfRef.__VdlyVal__pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem__v0 
                    = vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_din;
                vlSelfRef.__VdlyDim0__pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem__v0 
                    = vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_addr;
                vlSelfRef.__VdlySet__pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem__v0 = 1U;
            }
            vlSelfRef.pqc_accel_axi__DOT__bufb_dout 
                = vlSelfRef.pqc_accel_axi__DOT__u_buf__DOT__mem
                [(0x0000007fU & (((IData)(vlSelfRef.m_axis_tready) 
                                  & (IData)(vlSelfRef.m_axis_tvalid))
                                  ? (0x000001ffU & 
                                     (((IData)(1U) 
                                       + (IData)(vlSelfRef.pqc_accel_axi__DOT__rd_ptr)) 
                                      & (- (IData)(
                                                   (1U 
                                                    & (~ (IData)(vlSelfRef.m_axis_tlast)))))))
                                  : (IData)(vlSelfRef.pqc_accel_axi__DOT__rd_ptr)))];
            vlSelfRef.m_axis_tdata = vlSelfRef.pqc_accel_axi__DOT__bufb_dout;
        }
    }
    if ((3ULL & vlSelfRef.__VnbaTriggered[0U])) {
        Vpqc_accel_axi___024root___nba_sequent__TOP__1(vlSelf);
    }
    if ((5ULL & vlSelfRef.__VnbaTriggered[0U])) {
        Vpqc_accel_axi___024root___nba_sequent__TOP__2(vlSelf);
    }
    if ((3ULL & vlSelfRef.__VnbaTriggered[0U])) {
        Vpqc_accel_axi___024root___nba_sequent__TOP__3(vlSelf);
    }
    if ((1ULL & vlSelfRef.__VnbaTriggered[0U])) {
        {
            // Inlined CFunc: _nba_sequent__TOP__4
            vlSelfRef.pqc_accel_axi__DOT__bufa_dout 
                = vlSelfRef.pqc_accel_axi__DOT__u_buf__DOT__mem
                [vlSelfRef.pqc_accel_axi__DOT__bufa_addr];
            if (vlSelfRef.__VdlySet__pqc_accel_axi__DOT__u_buf__DOT__mem__v0) {
                vlSelfRef.pqc_accel_axi__DOT__u_buf__DOT__mem[vlSelfRef.__VdlyDim0__pqc_accel_axi__DOT__u_buf__DOT__mem__v0] 
                    = vlSelfRef.__VdlyVal__pqc_accel_axi__DOT__u_buf__DOT__mem__v0;
            }
        }
    }
    if ((5ULL & vlSelfRef.__VnbaTriggered[0U])) {
        Vpqc_accel_axi___024root___nba_sequent__TOP__5(vlSelf);
    }
    if ((7ULL & vlSelfRef.__VnbaTriggered[0U])) {
        Vpqc_accel_axi___024root___nba_comb__TOP__0(vlSelf);
    }
    if ((3ULL & vlSelfRef.__VnbaTriggered[0U])) {
        {
            // Inlined CFunc: _nba_sequent__TOP__6
            vlSelfRef.pqc_accel_axi__DOT__ntt_inverse 
                = vlSelfRef.__Vdly__pqc_accel_axi__DOT__ntt_inverse;
            vlSelfRef.pqc_accel_axi__DOT__ntt_start 
                = vlSelfRef.__Vdly__pqc_accel_axi__DOT__ntt_start;
            vlSelfRef.pqc_accel_axi__DOT__core_rst_n 
                = ((IData)(vlSelfRef.rst_n) && (1U 
                                                & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__soft_reset_r))));
            if (vlSelfRef.rst_n) {
                vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__soft_reset_r = 0U;
                if (vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_fire) {
                    if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_addr) 
                                  >> 4U)))) {
                        if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_addr) 
                                      >> 3U)))) {
                            if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_addr) 
                                          >> 2U)))) {
                                vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__soft_reset_r 
                                    = (1U & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_strb) 
                                             & (vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_data 
                                                >> 1U)));
                            }
                        }
                    }
                }
            } else {
                vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__soft_reset_r = 0U;
            }
            vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_addr 
                = ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__aw_hold)
                    ? (IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__aw_addr)
                    : (IData)(vlSelfRef.s_axi_awaddr));
            if (vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__w_hold) {
                vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_strb 
                    = vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__w_strb;
                vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_data 
                    = vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__w_data;
            } else {
                vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_strb 
                    = vlSelfRef.s_axi_wstrb;
                vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_data 
                    = vlSelfRef.s_axi_wdata;
            }
            vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_fire 
                = (((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__aw_hold) 
                    | (IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__aw_go)) 
                   & ((~ (IData)(vlSelfRef.s_axi_bvalid)) 
                      & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__w_hold) 
                         | (IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__w_go))));
        }
    }
    if ((1ULL & vlSelfRef.__VnbaTriggered[0U])) {
        {
            // Inlined CFunc: _nba_sequent__TOP__7
            vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_dout 
                = vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem
                [vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_addr];
            vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pb_dout 
                = vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem
                [vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pb_addr];
            if (vlSelfRef.__VdlySet__pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem__v0) {
                vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem[vlSelfRef.__VdlyDim0__pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem__v0] 
                    = vlSelfRef.__VdlyVal__pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem__v0;
            }
            if (vlSelfRef.__VdlySet__pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem__v1) {
                vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem[vlSelfRef.__VdlyDim0__pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem__v1] 
                    = vlSelfRef.__VdlyVal__pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem__v1;
            }
            vlSelfRef.__VdfgRegularize_hebeb780c_0_1 
                = (((- (IData)((1U & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_dout) 
                                      >> 0x0000000fU)))) 
                    << 0x00000010U) | (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_dout));
            vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__u_bf_gs_h__DOT__sum 
                = (0x0000ffffU & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_dout) 
                                  + (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pb_dout)));
            vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__u_bf_gs_h__DOT__diff 
                = (0x0000ffffU & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pb_dout) 
                                  - (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_dout)));
        }
    }
    if ((7ULL & vlSelfRef.__VnbaTriggered[0U])) {
        {
            // Inlined CFunc: _nba_comb__TOP__1
            vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_addr = 0U;
            vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pb_addr = 0U;
            if ((4U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state))) {
                if ((2U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state))) {
                    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_addr 
                        = (0x000000ffU & ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state))
                                           ? (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__scale_i)
                                           : (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__j)));
                    if ((1U & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state)))) {
                        vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pb_addr 
                            = (0x000000ffU & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__j_hi));
                    }
                } else if ((1U & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state)))) {
                    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_addr 
                        = (0x000000ffU & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__scale_i));
                }
            } else if ((2U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state))) {
                vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_addr 
                    = (0x000000ffU & ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state))
                                       ? (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__scale_i)
                                       : (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__j)));
                if ((1U & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state)))) {
                    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pb_addr 
                        = (0x000000ffU & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__j_hi));
                }
            } else if ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state))) {
                vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_addr 
                    = (0x000000ffU & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__j));
                vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pb_addr 
                    = (0x000000ffU & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__j_hi));
            } else {
                vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_addr 
                    = (0x000000ffU & (IData)(vlSelfRef.pqc_accel_axi__DOT__ntt_wr_addr));
                vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pb_addr 
                    = (0x000000ffU & (IData)(vlSelfRef.pqc_accel_axi__DOT__ntt_rd_addr));
            }
            vlSelfRef.pqc_accel_axi__DOT__bufa_din = 0U;
            if ((8U & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))) {
                if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__state) 
                              >> 2U)))) {
                    if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__state) 
                                  >> 1U)))) {
                        if ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))) {
                            vlSelfRef.pqc_accel_axi__DOT__bufa_din 
                                = vlSelfRef.pqc_accel_axi__DOT__sq_next;
                        }
                    }
                }
            } else if ((4U & (IData)(vlSelfRef.pqc_accel_axi__DOT__state))) {
                if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__state) 
                              >> 1U)))) {
                    if ((1U & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__state)))) {
                        vlSelfRef.pqc_accel_axi__DOT__bufa_din 
                            = ((IData)(vlSelfRef.pqc_accel_axi__DOT__is_ntt)
                                ? (((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pb_dout) 
                                    << 0x00000010U) 
                                   | (IData)(vlSelfRef.pqc_accel_axi__DOT__lo_lat))
                                : ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__cnt))
                                    ? (IData)((vlSelfRef.pqc_accel_axi__DOT__kec_rd_data 
                                               >> 0x20U))
                                    : (IData)(vlSelfRef.pqc_accel_axi__DOT__kec_rd_data)));
                    }
                }
            } else if ((1U & (~ ((IData)(vlSelfRef.pqc_accel_axi__DOT__state) 
                                 >> 1U)))) {
                if ((1U & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__state)))) {
                    vlSelfRef.pqc_accel_axi__DOT__bufa_din 
                        = vlSelfRef.s_axis_tdata;
                }
            }
        }
    }
}

void Vpqc_accel_axi___024root___trigger_orInto__act_vec_vec(VlUnpacked<QData/*63:0*/, 1> &out, const VlUnpacked<QData/*63:0*/, 1> &in) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vpqc_accel_axi___024root___trigger_orInto__act_vec_vec\n"); );
    // Locals
    IData/*31:0*/ n;
    // Body
    n = 0U;
    do {
        out[n] = (out[n] | in[n]);
        n = ((IData)(1U) + n);
    } while ((0U >= n));
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Vpqc_accel_axi___024root___dump_triggers__act(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag);
#endif  // VL_DEBUG

bool Vpqc_accel_axi___024root___eval_phase__act(Vpqc_accel_axi___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vpqc_accel_axi___024root___eval_phase__act\n"); );
    Vpqc_accel_axi__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    {
        // Inlined CFunc: _eval_triggers_vec__act
        vlSelfRef.__VactTriggered[0U] = (QData)((IData)(
                                                        ((((~ (IData)(vlSelfRef.pqc_accel_axi__DOT__core_rst_n)) 
                                                           & (IData)(vlSelfRef.__Vtrigprevexpr___TOP__pqc_accel_axi__DOT__core_rst_n__0)) 
                                                          << 2U) 
                                                         | ((((~ (IData)(vlSelfRef.rst_n)) 
                                                              & (IData)(vlSelfRef.__Vtrigprevexpr___TOP__rst_n__1)) 
                                                             << 1U) 
                                                            | ((IData)(vlSelfRef.clk) 
                                                               & (~ (IData)(vlSelfRef.__Vtrigprevexpr___TOP__clk__1)))))));
        vlSelfRef.__Vtrigprevexpr___TOP__clk__1 = vlSelfRef.clk;
        vlSelfRef.__Vtrigprevexpr___TOP__rst_n__1 = vlSelfRef.rst_n;
        vlSelfRef.__Vtrigprevexpr___TOP__pqc_accel_axi__DOT__core_rst_n__0 
            = vlSelfRef.pqc_accel_axi__DOT__core_rst_n;
    }
#ifdef VL_DEBUG
    if (VL_UNLIKELY(vlSymsp->_vm_contextp__->debug())) {
        Vpqc_accel_axi___024root___dump_triggers__act(vlSelfRef.__VactTriggered, "act"s);
    }
#endif
    Vpqc_accel_axi___024root___trigger_orInto__act_vec_vec(vlSelfRef.__VnbaTriggered, vlSelfRef.__VactTriggered);
    return (0U);
}

void Vpqc_accel_axi___024root___trigger_clear__act(VlUnpacked<QData/*63:0*/, 1> &out) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vpqc_accel_axi___024root___trigger_clear__act\n"); );
    // Locals
    IData/*31:0*/ n;
    // Body
    n = 0U;
    do {
        out[n] = 0ULL;
        n = ((IData)(1U) + n);
    } while ((1U > n));
}

bool Vpqc_accel_axi___024root___eval_phase__nba(Vpqc_accel_axi___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vpqc_accel_axi___024root___eval_phase__nba\n"); );
    Vpqc_accel_axi__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    CData/*0:0*/ __VnbaExecute;
    // Body
    __VnbaExecute = Vpqc_accel_axi___024root___trigger_anySet__act(vlSelfRef.__VnbaTriggered);
    if (__VnbaExecute) {
        Vpqc_accel_axi___024root___eval_nba(vlSelf);
        Vpqc_accel_axi___024root___trigger_clear__act(vlSelfRef.__VnbaTriggered);
    }
    return (__VnbaExecute);
}

void Vpqc_accel_axi___024root___eval(Vpqc_accel_axi___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vpqc_accel_axi___024root___eval\n"); );
    Vpqc_accel_axi__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    IData/*31:0*/ __VicoIterCount;
    IData/*31:0*/ __VnbaIterCount;
    // Body
    __VicoIterCount = 0U;
    do {
        if (VL_UNLIKELY(((0x00002710U < __VicoIterCount)))) {
#ifdef VL_DEBUG
            Vpqc_accel_axi___024root___dump_triggers__ico(vlSelfRef.__VicoTriggered, "ico"s);
#endif
            VL_FATAL_MT("/Volumes/Work/projects/code/pqc-hsm-fpga/hardware/rtl/bus/pqc_accel_axi.v", 43, "", "DIDNOTCONVERGE: Input combinational region did not converge after '--converge-limit' of 10000 tries");
        }
        __VicoIterCount = ((IData)(1U) + __VicoIterCount);
        vlSelfRef.__VicoPhaseResult = Vpqc_accel_axi___024root___eval_phase__ico(vlSelf);
    } while (vlSelfRef.__VicoPhaseResult);
    __VnbaIterCount = 0U;
    do {
        if (VL_UNLIKELY(((0x00002710U < __VnbaIterCount)))) {
#ifdef VL_DEBUG
            Vpqc_accel_axi___024root___dump_triggers__act(vlSelfRef.__VnbaTriggered, "nba"s);
#endif
            VL_FATAL_MT("/Volumes/Work/projects/code/pqc-hsm-fpga/hardware/rtl/bus/pqc_accel_axi.v", 43, "", "DIDNOTCONVERGE: NBA region did not converge after '--converge-limit' of 10000 tries");
        }
        __VnbaIterCount = ((IData)(1U) + __VnbaIterCount);
        vlSelfRef.__VactIterCount = 0U;
        do {
            if (VL_UNLIKELY(((0x00002710U < vlSelfRef.__VactIterCount)))) {
#ifdef VL_DEBUG
                Vpqc_accel_axi___024root___dump_triggers__act(vlSelfRef.__VactTriggered, "act"s);
#endif
                VL_FATAL_MT("/Volumes/Work/projects/code/pqc-hsm-fpga/hardware/rtl/bus/pqc_accel_axi.v", 43, "", "DIDNOTCONVERGE: Active region did not converge after '--converge-limit' of 10000 tries");
            }
            vlSelfRef.__VactIterCount = ((IData)(1U) 
                                         + vlSelfRef.__VactIterCount);
            vlSelfRef.__VactPhaseResult = Vpqc_accel_axi___024root___eval_phase__act(vlSelf);
        } while (vlSelfRef.__VactPhaseResult);
        vlSelfRef.__VnbaPhaseResult = Vpqc_accel_axi___024root___eval_phase__nba(vlSelf);
    } while (vlSelfRef.__VnbaPhaseResult);
}

#ifdef VL_DEBUG
void Vpqc_accel_axi___024root___eval_debug_assertions(Vpqc_accel_axi___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vpqc_accel_axi___024root___eval_debug_assertions\n"); );
    Vpqc_accel_axi__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    if (VL_UNLIKELY(((vlSelfRef.clk & 0xfeU)))) {
        Verilated::overWidthError("clk");
    }
    if (VL_UNLIKELY(((vlSelfRef.rst_n & 0xfeU)))) {
        Verilated::overWidthError("rst_n");
    }
    if (VL_UNLIKELY(((vlSelfRef.s_axi_awvalid & 0xfeU)))) {
        Verilated::overWidthError("s_axi_awvalid");
    }
    if (VL_UNLIKELY(((vlSelfRef.s_axi_wstrb & 0xf0U)))) {
        Verilated::overWidthError("s_axi_wstrb");
    }
    if (VL_UNLIKELY(((vlSelfRef.s_axi_wvalid & 0xfeU)))) {
        Verilated::overWidthError("s_axi_wvalid");
    }
    if (VL_UNLIKELY(((vlSelfRef.s_axi_bready & 0xfeU)))) {
        Verilated::overWidthError("s_axi_bready");
    }
    if (VL_UNLIKELY(((vlSelfRef.s_axi_arvalid & 0xfeU)))) {
        Verilated::overWidthError("s_axi_arvalid");
    }
    if (VL_UNLIKELY(((vlSelfRef.s_axi_rready & 0xfeU)))) {
        Verilated::overWidthError("s_axi_rready");
    }
    if (VL_UNLIKELY(((vlSelfRef.s_axis_tvalid & 0xfeU)))) {
        Verilated::overWidthError("s_axis_tvalid");
    }
    if (VL_UNLIKELY(((vlSelfRef.s_axis_tlast & 0xfeU)))) {
        Verilated::overWidthError("s_axis_tlast");
    }
    if (VL_UNLIKELY(((vlSelfRef.m_axis_tready & 0xfeU)))) {
        Verilated::overWidthError("m_axis_tready");
    }
}
#endif  // VL_DEBUG
