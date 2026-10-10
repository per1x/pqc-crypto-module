// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design implementation internals
// See Vntt_core.h for the primary calling header

#include "Vntt_core__pch.h"

bool Vntt_core___024root___trigger_anySet__ico(const VlUnpacked<QData/*63:0*/, 2> &in) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vntt_core___024root___trigger_anySet__ico\n"); );
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

void Vntt_core___024root___eval_ico(Vntt_core___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vntt_core___024root___eval_ico\n"); );
    Vntt_core__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    if ((0x0000000000000010ULL & vlSelfRef.__VicoTriggered[0U])) {
        {
            // Inlined CFunc: _ico_sequent__TOP__0
            vlSelfRef.ntt_core__DOT__pa_we = 0U;
            if ((4U & (IData)(vlSelfRef.ntt_core__DOT__state))) {
                if ((1U & (~ ((IData)(vlSelfRef.ntt_core__DOT__state) 
                              >> 1U)))) {
                    if ((1U & (~ (IData)(vlSelfRef.ntt_core__DOT__state)))) {
                        vlSelfRef.ntt_core__DOT__pa_we = 1U;
                    }
                }
            } else if ((2U & (IData)(vlSelfRef.ntt_core__DOT__state))) {
                if ((1U & (~ (IData)(vlSelfRef.ntt_core__DOT__state)))) {
                    vlSelfRef.ntt_core__DOT__pa_we = 1U;
                }
            } else if ((1U & (~ (IData)(vlSelfRef.ntt_core__DOT__state)))) {
                vlSelfRef.ntt_core__DOT__pa_we = vlSelfRef.wr_en;
            }
        }
    }
    if ((0x0000000000000080ULL & vlSelfRef.__VicoTriggered[0U])) {
        {
            // Inlined CFunc: _ico_sequent__TOP__1
            vlSelfRef.ntt_core__DOT__pb_addr = 0U;
            if ((4U & (IData)(vlSelfRef.ntt_core__DOT__state))) {
                if ((2U & (IData)(vlSelfRef.ntt_core__DOT__state))) {
                    if ((1U & (~ (IData)(vlSelfRef.ntt_core__DOT__state)))) {
                        vlSelfRef.ntt_core__DOT__pb_addr 
                            = (0x000000ffU & (IData)(vlSelfRef.ntt_core__DOT__j_hi));
                    }
                }
            } else if ((2U & (IData)(vlSelfRef.ntt_core__DOT__state))) {
                if ((1U & (~ (IData)(vlSelfRef.ntt_core__DOT__state)))) {
                    vlSelfRef.ntt_core__DOT__pb_addr 
                        = (0x000000ffU & (IData)(vlSelfRef.ntt_core__DOT__j_hi));
                }
            } else {
                vlSelfRef.ntt_core__DOT__pb_addr = 
                    (0x000000ffU & ((1U & (IData)(vlSelfRef.ntt_core__DOT__state))
                                     ? (IData)(vlSelfRef.ntt_core__DOT__j_hi)
                                     : (IData)(vlSelfRef.rd_addr)));
            }
        }
    }
    if ((0x0000000000000020ULL & vlSelfRef.__VicoTriggered[0U])) {
        {
            // Inlined CFunc: _ico_sequent__TOP__2
            vlSelfRef.ntt_core__DOT__pa_addr = 0U;
            if ((4U & (IData)(vlSelfRef.ntt_core__DOT__state))) {
                if ((2U & (IData)(vlSelfRef.ntt_core__DOT__state))) {
                    vlSelfRef.ntt_core__DOT__pa_addr 
                        = (0x000000ffU & ((1U & (IData)(vlSelfRef.ntt_core__DOT__state))
                                           ? (IData)(vlSelfRef.ntt_core__DOT__scale_i)
                                           : (IData)(vlSelfRef.ntt_core__DOT__j)));
                } else if ((1U & (~ (IData)(vlSelfRef.ntt_core__DOT__state)))) {
                    vlSelfRef.ntt_core__DOT__pa_addr 
                        = (0x000000ffU & (IData)(vlSelfRef.ntt_core__DOT__scale_i));
                }
            } else {
                vlSelfRef.ntt_core__DOT__pa_addr = 
                    (0x000000ffU & ((2U & (IData)(vlSelfRef.ntt_core__DOT__state))
                                     ? ((1U & (IData)(vlSelfRef.ntt_core__DOT__state))
                                         ? (IData)(vlSelfRef.ntt_core__DOT__scale_i)
                                         : (IData)(vlSelfRef.ntt_core__DOT__j))
                                     : ((1U & (IData)(vlSelfRef.ntt_core__DOT__state))
                                         ? (IData)(vlSelfRef.ntt_core__DOT__j)
                                         : (IData)(vlSelfRef.wr_addr))));
            }
        }
    }
    if ((0x0000000000000040ULL & vlSelfRef.__VicoTriggered[0U])) {
        {
            // Inlined CFunc: _ico_sequent__TOP__3
            vlSelfRef.ntt_core__DOT__pa_din = 0U;
            if ((4U & (IData)(vlSelfRef.ntt_core__DOT__state))) {
                if ((1U & (~ ((IData)(vlSelfRef.ntt_core__DOT__state) 
                              >> 1U)))) {
                    if ((1U & (~ (IData)(vlSelfRef.ntt_core__DOT__state)))) {
                        vlSelfRef.ntt_core__DOT__pa_din 
                            = (0x0000ffffU & ((IData)(vlSelfRef.ntt_core__DOT__inv_r)
                                               ? ((vlSelfRef.ntt_core__DOT__scprod_r 
                                                   - 
                                                   VL_MULS_III(32, (IData)(0x00000d01U), 
                                                               (((- (IData)(
                                                                            (1U 
                                                                             & (VL_MULS_III(16, (IData)(0xf301U), 
                                                                                (0x0000ffffU 
                                                                                & vlSelfRef.ntt_core__DOT__scprod_r)) 
                                                                                >> 0x0000000fU)))) 
                                                                 << 0x00000010U) 
                                                                | (0x0000ffffU 
                                                                   & VL_MULS_III(16, (IData)(0xf301U), 
                                                                                (0x0000ffffU 
                                                                                & vlSelfRef.ntt_core__DOT__scprod_r)))))) 
                                                  >> 0x00000010U)
                                               : (IData)(vlSelfRef.ntt_core__DOT__scbarr_r)));
                    }
                }
            } else if ((2U & (IData)(vlSelfRef.ntt_core__DOT__state))) {
                if ((1U & (~ (IData)(vlSelfRef.ntt_core__DOT__state)))) {
                    vlSelfRef.ntt_core__DOT__pa_din 
                        = (0x0000ffffU & ((IData)(vlSelfRef.ntt_core__DOT__inv_r)
                                           ? (IData)(vlSelfRef.ntt_core__DOT__gs_a_r)
                                           : ((IData)(vlSelfRef.ntt_core__DOT__bf_a_r) 
                                              + (IData)(vlSelfRef.ntt_core__DOT__u_bf_ct_t__DOT__t))));
                }
            } else if ((1U & (~ (IData)(vlSelfRef.ntt_core__DOT__state)))) {
                vlSelfRef.ntt_core__DOT__pa_din = vlSelfRef.wr_data;
            }
        }
    }
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Vntt_core___024root___dump_triggers__ico(const VlUnpacked<QData/*63:0*/, 2> &triggers, const std::string &tag);
#endif  // VL_DEBUG

bool Vntt_core___024root___eval_phase__ico(Vntt_core___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vntt_core___024root___eval_phase__ico\n"); );
    Vntt_core__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    CData/*0:0*/ __VicoExecute;
    // Body
    {
        // Inlined CFunc: _eval_triggers_vec__ico
        vlSelfRef.__VicoTriggered[0U] = (QData)((IData)(
                                                        (((((((IData)(vlSelfRef.rd_addr) 
                                                              != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__rd_addr__0)) 
                                                             << 3U) 
                                                            | (((IData)(vlSelfRef.wr_data) 
                                                                != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__wr_data__0)) 
                                                               << 2U)) 
                                                           | ((((IData)(vlSelfRef.wr_addr) 
                                                                != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__wr_addr__0)) 
                                                               << 1U) 
                                                              | ((IData)(vlSelfRef.wr_en) 
                                                                 != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__wr_en__0)))) 
                                                          << 4U) 
                                                         | (((((IData)(vlSelfRef.inverse) 
                                                               != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__inverse__0)) 
                                                              << 3U) 
                                                             | (((IData)(vlSelfRef.start) 
                                                                 != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__start__0)) 
                                                                << 2U)) 
                                                            | ((((IData)(vlSelfRef.rst_n) 
                                                                 != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__rst_n__0)) 
                                                                << 1U) 
                                                               | ((IData)(vlSelfRef.clk) 
                                                                  != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__clk__0)))))));
        vlSelfRef.__Vtrigprevexpr___TOP__clk__0 = vlSelfRef.clk;
        vlSelfRef.__Vtrigprevexpr___TOP__rst_n__0 = vlSelfRef.rst_n;
        vlSelfRef.__Vtrigprevexpr___TOP__start__0 = vlSelfRef.start;
        vlSelfRef.__Vtrigprevexpr___TOP__inverse__0 
            = vlSelfRef.inverse;
        vlSelfRef.__Vtrigprevexpr___TOP__wr_en__0 = vlSelfRef.wr_en;
        vlSelfRef.__Vtrigprevexpr___TOP__wr_addr__0 
            = vlSelfRef.wr_addr;
        vlSelfRef.__Vtrigprevexpr___TOP__wr_data__0 
            = vlSelfRef.wr_data;
        vlSelfRef.__Vtrigprevexpr___TOP__rd_addr__0 
            = vlSelfRef.rd_addr;
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
        }
    }
#ifdef VL_DEBUG
    if (VL_UNLIKELY(vlSymsp->_vm_contextp__->debug())) {
        Vntt_core___024root___dump_triggers__ico(vlSelfRef.__VicoTriggered, "ico"s);
    }
#endif
    __VicoExecute = Vntt_core___024root___trigger_anySet__ico(vlSelfRef.__VicoTriggered);
    if (__VicoExecute) {
        Vntt_core___024root___eval_ico(vlSelf);
    }
    return (__VicoExecute);
}

bool Vntt_core___024root___trigger_anySet__act(const VlUnpacked<QData/*63:0*/, 1> &in) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vntt_core___024root___trigger_anySet__act\n"); );
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

void Vntt_core___024root___nba_sequent__TOP__1(Vntt_core___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vntt_core___024root___nba_sequent__TOP__1\n"); );
    Vntt_core__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    CData/*2:0*/ __Vdly__ntt_core__DOT__state;
    __Vdly__ntt_core__DOT__state = 0;
    SData/*8:0*/ __Vdly__ntt_core__DOT__grp;
    __Vdly__ntt_core__DOT__grp = 0;
    SData/*8:0*/ __Vdly__ntt_core__DOT__len;
    __Vdly__ntt_core__DOT__len = 0;
    // Body
    __Vdly__ntt_core__DOT__grp = vlSelfRef.ntt_core__DOT__grp;
    __Vdly__ntt_core__DOT__len = vlSelfRef.ntt_core__DOT__len;
    __Vdly__ntt_core__DOT__state = vlSelfRef.ntt_core__DOT__state;
    if (vlSelfRef.rst_n) {
        if ((4U & (IData)(vlSelfRef.ntt_core__DOT__state))) {
            if ((2U & (IData)(vlSelfRef.ntt_core__DOT__state))) {
                if ((1U & (IData)(vlSelfRef.ntt_core__DOT__state))) {
                    vlSelfRef.ntt_core__DOT__scprod_r 
                        = VL_MULS_III(32, (IData)(0x000005a1U), vlSelfRef.__VdfgRegularize_hebeb780c_0_0);
                    vlSelfRef.ntt_core__DOT__scbarr_r 
                        = (0x0000ffffU & ((IData)(vlSelfRef.ntt_core__DOT__pa_dout) 
                                          - VL_MULS_III(16, (IData)(0x0d01U), 
                                                        (0x0000ffffU 
                                                         & VL_SHIFTRS_III(16,32,32, 
                                                                          ((IData)(0x02000000U) 
                                                                           + 
                                                                           VL_MULS_III(32, (IData)(0x00004ebfU), vlSelfRef.__VdfgRegularize_hebeb780c_0_0)), 0x0000001aU)))));
                    __Vdly__ntt_core__DOT__state = 4U;
                } else {
                    vlSelfRef.ntt_core__DOT__prod_r 
                        = ((IData)(vlSelfRef.ntt_core__DOT__inv_r)
                            ? VL_MULS_III(32, vlSelfRef.__VdfgRegularize_hebeb780c_0_1, 
                                          (((- (IData)(
                                                       (1U 
                                                        & ((IData)(vlSelfRef.ntt_core__DOT__u_bf_gs_h__DOT__diff) 
                                                           >> 0x0000000fU)))) 
                                            << 0x00000010U) 
                                           | (IData)(vlSelfRef.ntt_core__DOT__u_bf_gs_h__DOT__diff)))
                            : VL_MULS_III(32, vlSelfRef.__VdfgRegularize_hebeb780c_0_1, 
                                          (((- (IData)(
                                                       (1U 
                                                        & ((IData)(vlSelfRef.ntt_core__DOT__pb_dout) 
                                                           >> 0x0000000fU)))) 
                                            << 0x00000010U) 
                                           | (IData)(vlSelfRef.ntt_core__DOT__pb_dout))));
                    vlSelfRef.ntt_core__DOT__bf_a_r 
                        = vlSelfRef.ntt_core__DOT__pa_dout;
                    vlSelfRef.ntt_core__DOT__gs_a_r 
                        = (0x0000ffffU & ((IData)(vlSelfRef.ntt_core__DOT__u_bf_gs_h__DOT__sum) 
                                          - VL_MULS_III(16, (IData)(0x0d01U), 
                                                        (0x0000ffffU 
                                                         & VL_SHIFTRS_III(16,32,32, 
                                                                          ((IData)(0x02000000U) 
                                                                           + 
                                                                           VL_MULS_III(32, (IData)(0x00004ebfU), 
                                                                                (((- (IData)(
                                                                                (1U 
                                                                                & ((IData)(vlSelfRef.ntt_core__DOT__u_bf_gs_h__DOT__sum) 
                                                                                >> 0x0000000fU)))) 
                                                                                << 0x00000010U) 
                                                                                | (IData)(vlSelfRef.ntt_core__DOT__u_bf_gs_h__DOT__sum)))), 0x0000001aU)))));
                    __Vdly__ntt_core__DOT__state = 2U;
                }
            } else if ((1U & (IData)(vlSelfRef.ntt_core__DOT__state))) {
                vlSelfRef.done = 1U;
                __Vdly__ntt_core__DOT__state = 0U;
            } else if ((0x00ffU == (IData)(vlSelfRef.ntt_core__DOT__scale_i))) {
                __Vdly__ntt_core__DOT__state = 5U;
            } else {
                vlSelfRef.ntt_core__DOT__scale_i = 
                    (0x000001ffU & ((IData)(1U) + (IData)(vlSelfRef.ntt_core__DOT__scale_i)));
                __Vdly__ntt_core__DOT__state = 3U;
            }
        } else if ((2U & (IData)(vlSelfRef.ntt_core__DOT__state))) {
            if ((1U & (IData)(vlSelfRef.ntt_core__DOT__state))) {
                __Vdly__ntt_core__DOT__state = 7U;
            } else {
                __Vdly__ntt_core__DOT__state = 1U;
                if ((((IData)(1U) + (IData)(vlSelfRef.ntt_core__DOT__j)) 
                     < ((IData)(vlSelfRef.ntt_core__DOT__grp) 
                        + (IData)(vlSelfRef.ntt_core__DOT__len)))) {
                    vlSelfRef.ntt_core__DOT__j = (0x000001ffU 
                                                  & ((IData)(1U) 
                                                     + (IData)(vlSelfRef.ntt_core__DOT__j)));
                } else {
                    vlSelfRef.ntt_core__DOT__k = (0x000000ffU 
                                                  & ((IData)(vlSelfRef.ntt_core__DOT__inv_r)
                                                      ? 
                                                     ((IData)(vlSelfRef.ntt_core__DOT__k) 
                                                      - (IData)(1U))
                                                      : 
                                                     ((IData)(1U) 
                                                      + (IData)(vlSelfRef.ntt_core__DOT__k))));
                    if ((0x00000100U > ((IData)(vlSelfRef.ntt_core__DOT__grp) 
                                        + ((IData)(vlSelfRef.ntt_core__DOT__len) 
                                           << 1U)))) {
                        __Vdly__ntt_core__DOT__grp 
                            = (0x000001ffU & ((IData)(vlSelfRef.ntt_core__DOT__grp) 
                                              + ((IData)(vlSelfRef.ntt_core__DOT__len) 
                                                 << 1U)));
                        vlSelfRef.ntt_core__DOT__j 
                            = (0x000001ffU & ((IData)(vlSelfRef.ntt_core__DOT__grp) 
                                              + ((IData)(vlSelfRef.ntt_core__DOT__len) 
                                                 << 1U)));
                    } else if (vlSelfRef.ntt_core__DOT__inv_r) {
                        if ((0x0080U == (IData)(vlSelfRef.ntt_core__DOT__len))) {
                            vlSelfRef.ntt_core__DOT__scale_i = 0U;
                            __Vdly__ntt_core__DOT__state = 3U;
                        } else {
                            __Vdly__ntt_core__DOT__len 
                                = (0x000001ffU & ((IData)(vlSelfRef.ntt_core__DOT__len) 
                                                  << 1U));
                            __Vdly__ntt_core__DOT__grp = 0U;
                            vlSelfRef.ntt_core__DOT__j = 0U;
                        }
                    } else if ((2U == (IData)(vlSelfRef.ntt_core__DOT__len))) {
                        vlSelfRef.ntt_core__DOT__scale_i = 0U;
                        __Vdly__ntt_core__DOT__state = 3U;
                    } else {
                        __Vdly__ntt_core__DOT__len 
                            = ((IData)(vlSelfRef.ntt_core__DOT__len) 
                               >> 1U);
                        __Vdly__ntt_core__DOT__grp = 0U;
                        vlSelfRef.ntt_core__DOT__j = 0U;
                    }
                }
            }
        } else if ((1U & (IData)(vlSelfRef.ntt_core__DOT__state))) {
            __Vdly__ntt_core__DOT__state = 6U;
        } else if (vlSelfRef.start) {
            if (vlSelfRef.inverse) {
                __Vdly__ntt_core__DOT__len = 2U;
                vlSelfRef.ntt_core__DOT__k = 0x7fU;
                __Vdly__ntt_core__DOT__grp = 0U;
                vlSelfRef.ntt_core__DOT__j = 0U;
                vlSelfRef.done = 0U;
                vlSelfRef.ntt_core__DOT__inv_r = 1U;
            } else {
                __Vdly__ntt_core__DOT__len = 0x0080U;
                vlSelfRef.ntt_core__DOT__k = 1U;
                __Vdly__ntt_core__DOT__grp = 0U;
                vlSelfRef.ntt_core__DOT__j = 0U;
                vlSelfRef.done = 0U;
                vlSelfRef.ntt_core__DOT__inv_r = 0U;
            }
            __Vdly__ntt_core__DOT__state = 1U;
        }
    } else {
        __Vdly__ntt_core__DOT__len = 0x0080U;
        __Vdly__ntt_core__DOT__grp = 0U;
        vlSelfRef.ntt_core__DOT__j = 0U;
        vlSelfRef.ntt_core__DOT__k = 1U;
        vlSelfRef.ntt_core__DOT__scale_i = 0U;
        __Vdly__ntt_core__DOT__state = 0U;
        vlSelfRef.done = 0U;
        vlSelfRef.ntt_core__DOT__inv_r = 0U;
        vlSelfRef.ntt_core__DOT__prod_r = 0U;
        vlSelfRef.ntt_core__DOT__bf_a_r = 0U;
        vlSelfRef.ntt_core__DOT__gs_a_r = 0U;
        vlSelfRef.ntt_core__DOT__scprod_r = 0U;
        vlSelfRef.ntt_core__DOT__scbarr_r = 0U;
    }
    vlSelfRef.ntt_core__DOT__grp = __Vdly__ntt_core__DOT__grp;
    vlSelfRef.ntt_core__DOT__len = __Vdly__ntt_core__DOT__len;
    vlSelfRef.ntt_core__DOT__state = __Vdly__ntt_core__DOT__state;
    vlSelfRef.__VdfgRegularize_hebeb780c_0_1 = (((- (IData)(
                                                            (1U 
                                                             & (vlSelfRef.ntt_core__DOT__zetas
                                                                [
                                                                (0x0000007fU 
                                                                 & (IData)(vlSelfRef.ntt_core__DOT__k))] 
                                                                >> 0x0000000fU)))) 
                                                 << 0x00000010U) 
                                                | vlSelfRef.ntt_core__DOT__zetas
                                                [(0x0000007fU 
                                                  & (IData)(vlSelfRef.ntt_core__DOT__k))]);
    vlSelfRef.ntt_core__DOT__j_hi = (0x000001ffU & 
                                     ((IData)(vlSelfRef.ntt_core__DOT__j) 
                                      + (IData)(vlSelfRef.ntt_core__DOT__len)));
    vlSelfRef.ntt_core__DOT__u_bf_ct_t__DOT__t = ((vlSelfRef.ntt_core__DOT__prod_r 
                                                   - 
                                                   VL_MULS_III(32, (IData)(0x00000d01U), 
                                                               (((- (IData)(
                                                                            (1U 
                                                                             & (VL_MULS_III(16, (IData)(0xf301U), 
                                                                                (0x0000ffffU 
                                                                                & vlSelfRef.ntt_core__DOT__prod_r)) 
                                                                                >> 0x0000000fU)))) 
                                                                 << 0x00000010U) 
                                                                | (0x0000ffffU 
                                                                   & VL_MULS_III(16, (IData)(0xf301U), 
                                                                                (0x0000ffffU 
                                                                                & vlSelfRef.ntt_core__DOT__prod_r)))))) 
                                                  >> 0x00000010U);
    vlSelfRef.ntt_core__DOT__pb_we = 0U;
    vlSelfRef.ntt_core__DOT__pa_we = 0U;
    vlSelfRef.ntt_core__DOT__pb_din = 0U;
    if ((1U & (~ ((IData)(vlSelfRef.ntt_core__DOT__state) 
                  >> 2U)))) {
        if ((2U & (IData)(vlSelfRef.ntt_core__DOT__state))) {
            if ((1U & (~ (IData)(vlSelfRef.ntt_core__DOT__state)))) {
                vlSelfRef.ntt_core__DOT__pb_we = 1U;
                vlSelfRef.ntt_core__DOT__pb_din = (0x0000ffffU 
                                                   & ((IData)(vlSelfRef.ntt_core__DOT__inv_r)
                                                       ? (IData)(vlSelfRef.ntt_core__DOT__u_bf_ct_t__DOT__t)
                                                       : 
                                                      ((IData)(vlSelfRef.ntt_core__DOT__bf_a_r) 
                                                       - (IData)(vlSelfRef.ntt_core__DOT__u_bf_ct_t__DOT__t))));
            }
        }
    }
    vlSelfRef.ntt_core__DOT__pa_din = 0U;
    if ((4U & (IData)(vlSelfRef.ntt_core__DOT__state))) {
        if ((1U & (~ ((IData)(vlSelfRef.ntt_core__DOT__state) 
                      >> 1U)))) {
            if ((1U & (~ (IData)(vlSelfRef.ntt_core__DOT__state)))) {
                vlSelfRef.ntt_core__DOT__pa_we = 1U;
                vlSelfRef.ntt_core__DOT__pa_din = (0x0000ffffU 
                                                   & ((IData)(vlSelfRef.ntt_core__DOT__inv_r)
                                                       ? 
                                                      ((vlSelfRef.ntt_core__DOT__scprod_r 
                                                        - 
                                                        VL_MULS_III(32, (IData)(0x00000d01U), 
                                                                    (((- (IData)(
                                                                                (1U 
                                                                                & (VL_MULS_III(16, (IData)(0xf301U), 
                                                                                (0x0000ffffU 
                                                                                & vlSelfRef.ntt_core__DOT__scprod_r)) 
                                                                                >> 0x0000000fU)))) 
                                                                      << 0x00000010U) 
                                                                     | (0x0000ffffU 
                                                                        & VL_MULS_III(16, (IData)(0xf301U), 
                                                                                (0x0000ffffU 
                                                                                & vlSelfRef.ntt_core__DOT__scprod_r)))))) 
                                                       >> 0x00000010U)
                                                       : (IData)(vlSelfRef.ntt_core__DOT__scbarr_r)));
            }
        }
    } else if ((2U & (IData)(vlSelfRef.ntt_core__DOT__state))) {
        if ((1U & (~ (IData)(vlSelfRef.ntt_core__DOT__state)))) {
            vlSelfRef.ntt_core__DOT__pa_we = 1U;
            vlSelfRef.ntt_core__DOT__pa_din = (0x0000ffffU 
                                               & ((IData)(vlSelfRef.ntt_core__DOT__inv_r)
                                                   ? (IData)(vlSelfRef.ntt_core__DOT__gs_a_r)
                                                   : 
                                                  ((IData)(vlSelfRef.ntt_core__DOT__bf_a_r) 
                                                   + (IData)(vlSelfRef.ntt_core__DOT__u_bf_ct_t__DOT__t))));
        }
    } else if ((1U & (~ (IData)(vlSelfRef.ntt_core__DOT__state)))) {
        vlSelfRef.ntt_core__DOT__pa_we = vlSelfRef.wr_en;
        vlSelfRef.ntt_core__DOT__pa_din = vlSelfRef.wr_data;
    }
}

void Vntt_core___024root___eval_nba(Vntt_core___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vntt_core___024root___eval_nba\n"); );
    Vntt_core__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    if ((1ULL & vlSelfRef.__VnbaTriggered[0U])) {
        {
            // Inlined CFunc: _nba_sequent__TOP__0
            if (VL_UNLIKELY(((((IData)(vlSelfRef.ntt_core__DOT__pa_we) 
                               & (IData)(vlSelfRef.ntt_core__DOT__pb_we)) 
                              & ((IData)(vlSelfRef.ntt_core__DOT__pa_addr) 
                                 == (IData)(vlSelfRef.ntt_core__DOT__pb_addr)))))) {
                VL_WRITEF_NX("[%0t] ram_dp \346\226\255\350\250\200\345\244\261\350\264\245\357\274\232\344\270\244\344\270\252\345\217\243\345\220\214\346\227\266\345\206\231\345\220\214\344\270\200\345\234\260\345\235\200 %0d\n",3, 'T',-12
                             , '#',64,VL_TIME_UNITED_Q(1)
                             , '#',8,(IData)(vlSelfRef.ntt_core__DOT__pa_addr));
                VL_STOP_MT("/Volumes/Work/projects/code/pqc-hsm-fpga/hardware/rtl/common/ram_dp.v", 68, "");
            }
            vlSelfRef.__VdlySet__ntt_core__DOT__u_mem__DOT__mem__v1 = 0U;
            vlSelfRef.__VdlySet__ntt_core__DOT__u_mem__DOT__mem__v0 = 0U;
            if (vlSelfRef.ntt_core__DOT__pb_we) {
                vlSelfRef.__VdlyVal__ntt_core__DOT__u_mem__DOT__mem__v1 
                    = vlSelfRef.ntt_core__DOT__pb_din;
                vlSelfRef.__VdlyDim0__ntt_core__DOT__u_mem__DOT__mem__v1 
                    = vlSelfRef.ntt_core__DOT__pb_addr;
                vlSelfRef.__VdlySet__ntt_core__DOT__u_mem__DOT__mem__v1 = 1U;
            }
            if (vlSelfRef.ntt_core__DOT__pa_we) {
                vlSelfRef.__VdlyVal__ntt_core__DOT__u_mem__DOT__mem__v0 
                    = vlSelfRef.ntt_core__DOT__pa_din;
                vlSelfRef.__VdlyDim0__ntt_core__DOT__u_mem__DOT__mem__v0 
                    = vlSelfRef.ntt_core__DOT__pa_addr;
                vlSelfRef.__VdlySet__ntt_core__DOT__u_mem__DOT__mem__v0 = 1U;
            }
        }
    }
    if ((3ULL & vlSelfRef.__VnbaTriggered[0U])) {
        Vntt_core___024root___nba_sequent__TOP__1(vlSelf);
    }
    if ((1ULL & vlSelfRef.__VnbaTriggered[0U])) {
        {
            // Inlined CFunc: _nba_sequent__TOP__2
            vlSelfRef.ntt_core__DOT__pb_dout = vlSelfRef.ntt_core__DOT__u_mem__DOT__mem
                [vlSelfRef.ntt_core__DOT__pb_addr];
            vlSelfRef.ntt_core__DOT__pa_dout = vlSelfRef.ntt_core__DOT__u_mem__DOT__mem
                [vlSelfRef.ntt_core__DOT__pa_addr];
            if (vlSelfRef.__VdlySet__ntt_core__DOT__u_mem__DOT__mem__v0) {
                vlSelfRef.ntt_core__DOT__u_mem__DOT__mem[vlSelfRef.__VdlyDim0__ntt_core__DOT__u_mem__DOT__mem__v0] 
                    = vlSelfRef.__VdlyVal__ntt_core__DOT__u_mem__DOT__mem__v0;
            }
            if (vlSelfRef.__VdlySet__ntt_core__DOT__u_mem__DOT__mem__v1) {
                vlSelfRef.ntt_core__DOT__u_mem__DOT__mem[vlSelfRef.__VdlyDim0__ntt_core__DOT__u_mem__DOT__mem__v1] 
                    = vlSelfRef.__VdlyVal__ntt_core__DOT__u_mem__DOT__mem__v1;
            }
            vlSelfRef.rd_data = vlSelfRef.ntt_core__DOT__pb_dout;
            vlSelfRef.__VdfgRegularize_hebeb780c_0_0 
                = (((- (IData)((1U & ((IData)(vlSelfRef.ntt_core__DOT__pa_dout) 
                                      >> 0x0000000fU)))) 
                    << 0x00000010U) | (IData)(vlSelfRef.ntt_core__DOT__pa_dout));
            vlSelfRef.ntt_core__DOT__u_bf_gs_h__DOT__sum 
                = (0x0000ffffU & ((IData)(vlSelfRef.ntt_core__DOT__pa_dout) 
                                  + (IData)(vlSelfRef.ntt_core__DOT__pb_dout)));
            vlSelfRef.ntt_core__DOT__u_bf_gs_h__DOT__diff 
                = (0x0000ffffU & ((IData)(vlSelfRef.ntt_core__DOT__pb_dout) 
                                  - (IData)(vlSelfRef.ntt_core__DOT__pa_dout)));
        }
    }
    if ((3ULL & vlSelfRef.__VnbaTriggered[0U])) {
        {
            // Inlined CFunc: _nba_sequent__TOP__3
            vlSelfRef.ntt_core__DOT__pb_addr = 0U;
            vlSelfRef.ntt_core__DOT__pa_addr = 0U;
            if ((4U & (IData)(vlSelfRef.ntt_core__DOT__state))) {
                if ((2U & (IData)(vlSelfRef.ntt_core__DOT__state))) {
                    if ((1U & (~ (IData)(vlSelfRef.ntt_core__DOT__state)))) {
                        vlSelfRef.ntt_core__DOT__pb_addr 
                            = (0x000000ffU & (IData)(vlSelfRef.ntt_core__DOT__j_hi));
                    }
                    vlSelfRef.ntt_core__DOT__pa_addr 
                        = (0x000000ffU & ((1U & (IData)(vlSelfRef.ntt_core__DOT__state))
                                           ? (IData)(vlSelfRef.ntt_core__DOT__scale_i)
                                           : (IData)(vlSelfRef.ntt_core__DOT__j)));
                } else if ((1U & (~ (IData)(vlSelfRef.ntt_core__DOT__state)))) {
                    vlSelfRef.ntt_core__DOT__pa_addr 
                        = (0x000000ffU & (IData)(vlSelfRef.ntt_core__DOT__scale_i));
                }
            } else if ((2U & (IData)(vlSelfRef.ntt_core__DOT__state))) {
                if ((1U & (~ (IData)(vlSelfRef.ntt_core__DOT__state)))) {
                    vlSelfRef.ntt_core__DOT__pb_addr 
                        = (0x000000ffU & (IData)(vlSelfRef.ntt_core__DOT__j_hi));
                }
                vlSelfRef.ntt_core__DOT__pa_addr = 
                    (0x000000ffU & ((1U & (IData)(vlSelfRef.ntt_core__DOT__state))
                                     ? (IData)(vlSelfRef.ntt_core__DOT__scale_i)
                                     : (IData)(vlSelfRef.ntt_core__DOT__j)));
            } else if ((1U & (IData)(vlSelfRef.ntt_core__DOT__state))) {
                vlSelfRef.ntt_core__DOT__pb_addr = 
                    (0x000000ffU & (IData)(vlSelfRef.ntt_core__DOT__j_hi));
                vlSelfRef.ntt_core__DOT__pa_addr = 
                    (0x000000ffU & (IData)(vlSelfRef.ntt_core__DOT__j));
            } else {
                vlSelfRef.ntt_core__DOT__pb_addr = 
                    (0x000000ffU & (IData)(vlSelfRef.rd_addr));
                vlSelfRef.ntt_core__DOT__pa_addr = 
                    (0x000000ffU & (IData)(vlSelfRef.wr_addr));
            }
        }
    }
}

void Vntt_core___024root___trigger_orInto__act_vec_vec(VlUnpacked<QData/*63:0*/, 1> &out, const VlUnpacked<QData/*63:0*/, 1> &in) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vntt_core___024root___trigger_orInto__act_vec_vec\n"); );
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
VL_ATTR_COLD void Vntt_core___024root___dump_triggers__act(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag);
#endif  // VL_DEBUG

bool Vntt_core___024root___eval_phase__act(Vntt_core___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vntt_core___024root___eval_phase__act\n"); );
    Vntt_core__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    {
        // Inlined CFunc: _eval_triggers_vec__act
        vlSelfRef.__VactTriggered[0U] = (QData)((IData)(
                                                        ((((~ (IData)(vlSelfRef.rst_n)) 
                                                           & (IData)(vlSelfRef.__Vtrigprevexpr___TOP__rst_n__1)) 
                                                          << 1U) 
                                                         | ((IData)(vlSelfRef.clk) 
                                                            & (~ (IData)(vlSelfRef.__Vtrigprevexpr___TOP__clk__1))))));
        vlSelfRef.__Vtrigprevexpr___TOP__clk__1 = vlSelfRef.clk;
        vlSelfRef.__Vtrigprevexpr___TOP__rst_n__1 = vlSelfRef.rst_n;
    }
#ifdef VL_DEBUG
    if (VL_UNLIKELY(vlSymsp->_vm_contextp__->debug())) {
        Vntt_core___024root___dump_triggers__act(vlSelfRef.__VactTriggered, "act"s);
    }
#endif
    Vntt_core___024root___trigger_orInto__act_vec_vec(vlSelfRef.__VnbaTriggered, vlSelfRef.__VactTriggered);
    return (0U);
}

void Vntt_core___024root___trigger_clear__act(VlUnpacked<QData/*63:0*/, 1> &out) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vntt_core___024root___trigger_clear__act\n"); );
    // Locals
    IData/*31:0*/ n;
    // Body
    n = 0U;
    do {
        out[n] = 0ULL;
        n = ((IData)(1U) + n);
    } while ((1U > n));
}

bool Vntt_core___024root___eval_phase__nba(Vntt_core___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vntt_core___024root___eval_phase__nba\n"); );
    Vntt_core__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    CData/*0:0*/ __VnbaExecute;
    // Body
    __VnbaExecute = Vntt_core___024root___trigger_anySet__act(vlSelfRef.__VnbaTriggered);
    if (__VnbaExecute) {
        Vntt_core___024root___eval_nba(vlSelf);
        Vntt_core___024root___trigger_clear__act(vlSelfRef.__VnbaTriggered);
    }
    return (__VnbaExecute);
}

void Vntt_core___024root___eval(Vntt_core___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vntt_core___024root___eval\n"); );
    Vntt_core__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    IData/*31:0*/ __VicoIterCount;
    IData/*31:0*/ __VnbaIterCount;
    // Body
    __VicoIterCount = 0U;
    do {
        if (VL_UNLIKELY(((0x00002710U < __VicoIterCount)))) {
#ifdef VL_DEBUG
            Vntt_core___024root___dump_triggers__ico(vlSelfRef.__VicoTriggered, "ico"s);
#endif
            VL_FATAL_MT("/Volumes/Work/projects/code/pqc-hsm-fpga/hardware/rtl/mlkem/ntt_core.v", 37, "", "DIDNOTCONVERGE: Input combinational region did not converge after '--converge-limit' of 10000 tries");
        }
        __VicoIterCount = ((IData)(1U) + __VicoIterCount);
        vlSelfRef.__VicoPhaseResult = Vntt_core___024root___eval_phase__ico(vlSelf);
    } while (vlSelfRef.__VicoPhaseResult);
    __VnbaIterCount = 0U;
    do {
        if (VL_UNLIKELY(((0x00002710U < __VnbaIterCount)))) {
#ifdef VL_DEBUG
            Vntt_core___024root___dump_triggers__act(vlSelfRef.__VnbaTriggered, "nba"s);
#endif
            VL_FATAL_MT("/Volumes/Work/projects/code/pqc-hsm-fpga/hardware/rtl/mlkem/ntt_core.v", 37, "", "DIDNOTCONVERGE: NBA region did not converge after '--converge-limit' of 10000 tries");
        }
        __VnbaIterCount = ((IData)(1U) + __VnbaIterCount);
        vlSelfRef.__VactIterCount = 0U;
        do {
            if (VL_UNLIKELY(((0x00002710U < vlSelfRef.__VactIterCount)))) {
#ifdef VL_DEBUG
                Vntt_core___024root___dump_triggers__act(vlSelfRef.__VactTriggered, "act"s);
#endif
                VL_FATAL_MT("/Volumes/Work/projects/code/pqc-hsm-fpga/hardware/rtl/mlkem/ntt_core.v", 37, "", "DIDNOTCONVERGE: Active region did not converge after '--converge-limit' of 10000 tries");
            }
            vlSelfRef.__VactIterCount = ((IData)(1U) 
                                         + vlSelfRef.__VactIterCount);
            vlSelfRef.__VactPhaseResult = Vntt_core___024root___eval_phase__act(vlSelf);
        } while (vlSelfRef.__VactPhaseResult);
        vlSelfRef.__VnbaPhaseResult = Vntt_core___024root___eval_phase__nba(vlSelf);
    } while (vlSelfRef.__VnbaPhaseResult);
}

#ifdef VL_DEBUG
void Vntt_core___024root___eval_debug_assertions(Vntt_core___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vntt_core___024root___eval_debug_assertions\n"); );
    Vntt_core__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    if (VL_UNLIKELY(((vlSelfRef.clk & 0xfeU)))) {
        Verilated::overWidthError("clk");
    }
    if (VL_UNLIKELY(((vlSelfRef.rst_n & 0xfeU)))) {
        Verilated::overWidthError("rst_n");
    }
    if (VL_UNLIKELY(((vlSelfRef.start & 0xfeU)))) {
        Verilated::overWidthError("start");
    }
    if (VL_UNLIKELY(((vlSelfRef.inverse & 0xfeU)))) {
        Verilated::overWidthError("inverse");
    }
    if (VL_UNLIKELY(((vlSelfRef.wr_en & 0xfeU)))) {
        Verilated::overWidthError("wr_en");
    }
}
#endif  // VL_DEBUG
