// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design implementation internals
// See Vkeccak_f1600.h for the primary calling header

#include "Vkeccak_f1600__pch.h"

VL_ATTR_COLD void Vkeccak_f1600___024root___eval_static(Vkeccak_f1600___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vkeccak_f1600___024root___eval_static\n"); );
    Vkeccak_f1600__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    vlSelfRef.__Vtrigprevexpr___TOP__clk__0 = vlSelfRef.clk;
    vlSelfRef.__Vtrigprevexpr___TOP__rst_n__0 = vlSelfRef.rst_n;
    vlSelfRef.__Vtrigprevexpr___TOP__start__0 = vlSelfRef.start;
    vlSelfRef.__Vtrigprevexpr___TOP__wr_en__0 = vlSelfRef.wr_en;
    vlSelfRef.__Vtrigprevexpr___TOP__wr_addr__0 = vlSelfRef.wr_addr;
    vlSelfRef.__Vtrigprevexpr___TOP__wr_data__0 = vlSelfRef.wr_data;
    vlSelfRef.__Vtrigprevexpr___TOP__rd_addr__0 = vlSelfRef.rd_addr;
    vlSelfRef.__Vtrigprevexpr___TOP__clk__1 = vlSelfRef.clk;
    vlSelfRef.__Vtrigprevexpr___TOP__rst_n__1 = vlSelfRef.rst_n;
}

VL_ATTR_COLD void Vkeccak_f1600___024root___eval_initial(Vkeccak_f1600___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vkeccak_f1600___024root___eval_initial\n"); );
    Vkeccak_f1600__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

VL_ATTR_COLD void Vkeccak_f1600___024root___eval_final(Vkeccak_f1600___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vkeccak_f1600___024root___eval_final\n"); );
    Vkeccak_f1600__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Vkeccak_f1600___024root___dump_triggers__stl(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag);
#endif  // VL_DEBUG
VL_ATTR_COLD bool Vkeccak_f1600___024root___eval_phase__stl(Vkeccak_f1600___024root* vlSelf);

VL_ATTR_COLD void Vkeccak_f1600___024root___eval_settle(Vkeccak_f1600___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vkeccak_f1600___024root___eval_settle\n"); );
    Vkeccak_f1600__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    IData/*31:0*/ __VstlIterCount;
    // Body
    __VstlIterCount = 0U;
    vlSelfRef.__VstlFirstIteration = 1U;
    do {
        if (VL_UNLIKELY(((0x00002710U < __VstlIterCount)))) {
#ifdef VL_DEBUG
            Vkeccak_f1600___024root___dump_triggers__stl(vlSelfRef.__VstlTriggered, "stl"s);
#endif
            VL_FATAL_MT("/Volumes/Work/projects/code/pqc-hsm-fpga/hardware/rtl/keccak/keccak_f1600.v", 20, "", "DIDNOTCONVERGE: Settle region did not converge after '--converge-limit' of 10000 tries");
        }
        __VstlIterCount = ((IData)(1U) + __VstlIterCount);
        vlSelfRef.__VstlPhaseResult = Vkeccak_f1600___024root___eval_phase__stl(vlSelf);
        vlSelfRef.__VstlFirstIteration = 0U;
    } while (vlSelfRef.__VstlPhaseResult);
}

VL_ATTR_COLD bool Vkeccak_f1600___024root___trigger_anySet__stl(const VlUnpacked<QData/*63:0*/, 1> &in);

#ifdef VL_DEBUG
VL_ATTR_COLD void Vkeccak_f1600___024root___dump_triggers__stl(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vkeccak_f1600___024root___dump_triggers__stl\n"); );
    // Body
    if ((1U & (~ (IData)(Vkeccak_f1600___024root___trigger_anySet__stl(triggers))))) {
        VL_DBG_MSGS("         No '" + tag + "' region triggers active\n");
    }
    if ((1U & (IData)(triggers[0U]))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 0 is active: Internal 'stl' trigger - first iteration\n");
    }
}
#endif  // VL_DEBUG

VL_ATTR_COLD bool Vkeccak_f1600___024root___trigger_anySet__stl(const VlUnpacked<QData/*63:0*/, 1> &in) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vkeccak_f1600___024root___trigger_anySet__stl\n"); );
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

extern const VlWide<64>/*2047:0*/ Vkeccak_f1600__ConstPool__CONST_hd522b744_0;

VL_ATTR_COLD void Vkeccak_f1600___024root___stl_sequent__TOP__0(Vkeccak_f1600___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vkeccak_f1600___024root___stl_sequent__TOP__0\n"); );
    Vkeccak_f1600__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    QData/*63:0*/ keccak_f1600__DOT____VlemCall_3__rc;
    QData/*63:0*/ keccak_f1600__DOT____VlemCall_2__rotl;
    QData/*63:0*/ keccak_f1600__DOT____VlemCall_0__rotl;
    QData/*63:0*/ __Vfunc_keccak_f1600__DOT__rotl__0__x;
    __Vfunc_keccak_f1600__DOT__rotl__0__x = 0;
    QData/*63:0*/ __Vfunc_keccak_f1600__DOT__rotl__2__x;
    __Vfunc_keccak_f1600__DOT__rotl__2__x = 0;
    CData/*4:0*/ __Vfunc_keccak_f1600__DOT__rc__3__r;
    __Vfunc_keccak_f1600__DOT__rc__3__r = 0;
    // Body
    vlSelfRef.rd_data = (vlSelfRef.keccak_f1600__DOT__A
                         [vlSelfRef.rd_addr] & (- (QData)((IData)(
                                                                  (0x18U 
                                                                   >= (IData)(vlSelfRef.rd_addr))))));
    vlSelfRef.keccak_f1600__DOT__C[0U] = ((((vlSelfRef.keccak_f1600__DOT__A[0U] 
                                             ^ vlSelfRef.keccak_f1600__DOT__A[5U]) 
                                            ^ vlSelfRef.keccak_f1600__DOT__A[10U]) 
                                           ^ vlSelfRef.keccak_f1600__DOT__A[15U]) 
                                          ^ vlSelfRef.keccak_f1600__DOT__A[20U]);
    vlSelfRef.keccak_f1600__DOT__C[1U] = ((((vlSelfRef.keccak_f1600__DOT__A[1U] 
                                             ^ vlSelfRef.keccak_f1600__DOT__A[6U]) 
                                            ^ vlSelfRef.keccak_f1600__DOT__A[11U]) 
                                           ^ vlSelfRef.keccak_f1600__DOT__A[16U]) 
                                          ^ vlSelfRef.keccak_f1600__DOT__A[21U]);
    vlSelfRef.keccak_f1600__DOT__C[2U] = ((((vlSelfRef.keccak_f1600__DOT__A[2U] 
                                             ^ vlSelfRef.keccak_f1600__DOT__A[7U]) 
                                            ^ vlSelfRef.keccak_f1600__DOT__A[12U]) 
                                           ^ vlSelfRef.keccak_f1600__DOT__A[17U]) 
                                          ^ vlSelfRef.keccak_f1600__DOT__A[22U]);
    vlSelfRef.keccak_f1600__DOT__C[3U] = ((((vlSelfRef.keccak_f1600__DOT__A[3U] 
                                             ^ vlSelfRef.keccak_f1600__DOT__A[8U]) 
                                            ^ vlSelfRef.keccak_f1600__DOT__A[13U]) 
                                           ^ vlSelfRef.keccak_f1600__DOT__A[18U]) 
                                          ^ vlSelfRef.keccak_f1600__DOT__A[23U]);
    vlSelfRef.keccak_f1600__DOT__C[4U] = ((((vlSelfRef.keccak_f1600__DOT__A[4U] 
                                             ^ vlSelfRef.keccak_f1600__DOT__A[9U]) 
                                            ^ vlSelfRef.keccak_f1600__DOT__A[14U]) 
                                           ^ vlSelfRef.keccak_f1600__DOT__A[19U]) 
                                          ^ vlSelfRef.keccak_f1600__DOT__A[24U]);
    __Vfunc_keccak_f1600__DOT__rotl__0__x = vlSelfRef.keccak_f1600__DOT__C[1U];
    keccak_f1600__DOT____VlemCall_0__rotl = ((__Vfunc_keccak_f1600__DOT__rotl__0__x 
                                              << 1U) 
                                             | (__Vfunc_keccak_f1600__DOT__rotl__0__x 
                                                >> 0x0000003fU));
    vlSelfRef.keccak_f1600__DOT__D[0U] = (vlSelfRef.keccak_f1600__DOT__C[4U] 
                                          ^ keccak_f1600__DOT____VlemCall_0__rotl);
    __Vfunc_keccak_f1600__DOT__rotl__0__x = vlSelfRef.keccak_f1600__DOT__C[2U];
    keccak_f1600__DOT____VlemCall_0__rotl = ((__Vfunc_keccak_f1600__DOT__rotl__0__x 
                                              << 1U) 
                                             | (__Vfunc_keccak_f1600__DOT__rotl__0__x 
                                                >> 0x0000003fU));
    vlSelfRef.keccak_f1600__DOT__D[1U] = (vlSelfRef.keccak_f1600__DOT__C[0U] 
                                          ^ keccak_f1600__DOT____VlemCall_0__rotl);
    __Vfunc_keccak_f1600__DOT__rotl__0__x = vlSelfRef.keccak_f1600__DOT__C[3U];
    keccak_f1600__DOT____VlemCall_0__rotl = ((__Vfunc_keccak_f1600__DOT__rotl__0__x 
                                              << 1U) 
                                             | (__Vfunc_keccak_f1600__DOT__rotl__0__x 
                                                >> 0x0000003fU));
    vlSelfRef.keccak_f1600__DOT__D[2U] = (vlSelfRef.keccak_f1600__DOT__C[1U] 
                                          ^ keccak_f1600__DOT____VlemCall_0__rotl);
    __Vfunc_keccak_f1600__DOT__rotl__0__x = vlSelfRef.keccak_f1600__DOT__C[4U];
    keccak_f1600__DOT____VlemCall_0__rotl = ((__Vfunc_keccak_f1600__DOT__rotl__0__x 
                                              << 1U) 
                                             | (__Vfunc_keccak_f1600__DOT__rotl__0__x 
                                                >> 0x0000003fU));
    vlSelfRef.keccak_f1600__DOT__D[3U] = (vlSelfRef.keccak_f1600__DOT__C[2U] 
                                          ^ keccak_f1600__DOT____VlemCall_0__rotl);
    __Vfunc_keccak_f1600__DOT__rotl__0__x = vlSelfRef.keccak_f1600__DOT__C[0U];
    keccak_f1600__DOT____VlemCall_0__rotl = ((__Vfunc_keccak_f1600__DOT__rotl__0__x 
                                              << 1U) 
                                             | (__Vfunc_keccak_f1600__DOT__rotl__0__x 
                                                >> 0x0000003fU));
    vlSelfRef.keccak_f1600__DOT__D[4U] = (vlSelfRef.keccak_f1600__DOT__C[3U] 
                                          ^ keccak_f1600__DOT____VlemCall_0__rotl);
    vlSelfRef.keccak_f1600__DOT__Ath[0U] = (vlSelfRef.keccak_f1600__DOT__A[0U] 
                                            ^ vlSelfRef.keccak_f1600__DOT__D[0U]);
    vlSelfRef.keccak_f1600__DOT__Ath[5U] = (vlSelfRef.keccak_f1600__DOT__A[5U] 
                                            ^ vlSelfRef.keccak_f1600__DOT__D[0U]);
    vlSelfRef.keccak_f1600__DOT__Ath[10U] = (vlSelfRef.keccak_f1600__DOT__A[10U] 
                                             ^ vlSelfRef.keccak_f1600__DOT__D[0U]);
    vlSelfRef.keccak_f1600__DOT__Ath[15U] = (vlSelfRef.keccak_f1600__DOT__A[15U] 
                                             ^ vlSelfRef.keccak_f1600__DOT__D[0U]);
    vlSelfRef.keccak_f1600__DOT__Ath[20U] = (vlSelfRef.keccak_f1600__DOT__A[20U] 
                                             ^ vlSelfRef.keccak_f1600__DOT__D[0U]);
    vlSelfRef.keccak_f1600__DOT__Ath[1U] = (vlSelfRef.keccak_f1600__DOT__A[1U] 
                                            ^ vlSelfRef.keccak_f1600__DOT__D[1U]);
    vlSelfRef.keccak_f1600__DOT__Ath[6U] = (vlSelfRef.keccak_f1600__DOT__A[6U] 
                                            ^ vlSelfRef.keccak_f1600__DOT__D[1U]);
    vlSelfRef.keccak_f1600__DOT__Ath[11U] = (vlSelfRef.keccak_f1600__DOT__A[11U] 
                                             ^ vlSelfRef.keccak_f1600__DOT__D[1U]);
    vlSelfRef.keccak_f1600__DOT__Ath[16U] = (vlSelfRef.keccak_f1600__DOT__A[16U] 
                                             ^ vlSelfRef.keccak_f1600__DOT__D[1U]);
    vlSelfRef.keccak_f1600__DOT__Ath[21U] = (vlSelfRef.keccak_f1600__DOT__A[21U] 
                                             ^ vlSelfRef.keccak_f1600__DOT__D[1U]);
    vlSelfRef.keccak_f1600__DOT__Ath[2U] = (vlSelfRef.keccak_f1600__DOT__A[2U] 
                                            ^ vlSelfRef.keccak_f1600__DOT__D[2U]);
    vlSelfRef.keccak_f1600__DOT__Ath[7U] = (vlSelfRef.keccak_f1600__DOT__A[7U] 
                                            ^ vlSelfRef.keccak_f1600__DOT__D[2U]);
    vlSelfRef.keccak_f1600__DOT__Ath[12U] = (vlSelfRef.keccak_f1600__DOT__A[12U] 
                                             ^ vlSelfRef.keccak_f1600__DOT__D[2U]);
    vlSelfRef.keccak_f1600__DOT__Ath[17U] = (vlSelfRef.keccak_f1600__DOT__A[17U] 
                                             ^ vlSelfRef.keccak_f1600__DOT__D[2U]);
    vlSelfRef.keccak_f1600__DOT__Ath[22U] = (vlSelfRef.keccak_f1600__DOT__A[22U] 
                                             ^ vlSelfRef.keccak_f1600__DOT__D[2U]);
    vlSelfRef.keccak_f1600__DOT__Ath[3U] = (vlSelfRef.keccak_f1600__DOT__A[3U] 
                                            ^ vlSelfRef.keccak_f1600__DOT__D[3U]);
    vlSelfRef.keccak_f1600__DOT__Ath[8U] = (vlSelfRef.keccak_f1600__DOT__A[8U] 
                                            ^ vlSelfRef.keccak_f1600__DOT__D[3U]);
    vlSelfRef.keccak_f1600__DOT__Ath[13U] = (vlSelfRef.keccak_f1600__DOT__A[13U] 
                                             ^ vlSelfRef.keccak_f1600__DOT__D[3U]);
    vlSelfRef.keccak_f1600__DOT__Ath[18U] = (vlSelfRef.keccak_f1600__DOT__A[18U] 
                                             ^ vlSelfRef.keccak_f1600__DOT__D[3U]);
    vlSelfRef.keccak_f1600__DOT__Ath[23U] = (vlSelfRef.keccak_f1600__DOT__A[23U] 
                                             ^ vlSelfRef.keccak_f1600__DOT__D[3U]);
    vlSelfRef.keccak_f1600__DOT__Ath[4U] = (vlSelfRef.keccak_f1600__DOT__A[4U] 
                                            ^ vlSelfRef.keccak_f1600__DOT__D[4U]);
    vlSelfRef.keccak_f1600__DOT__Ath[9U] = (vlSelfRef.keccak_f1600__DOT__A[9U] 
                                            ^ vlSelfRef.keccak_f1600__DOT__D[4U]);
    vlSelfRef.keccak_f1600__DOT__Ath[14U] = (vlSelfRef.keccak_f1600__DOT__A[14U] 
                                             ^ vlSelfRef.keccak_f1600__DOT__D[4U]);
    vlSelfRef.keccak_f1600__DOT__Ath[19U] = (vlSelfRef.keccak_f1600__DOT__A[19U] 
                                             ^ vlSelfRef.keccak_f1600__DOT__D[4U]);
    vlSelfRef.keccak_f1600__DOT__Ath[24U] = (vlSelfRef.keccak_f1600__DOT__A[24U] 
                                             ^ vlSelfRef.keccak_f1600__DOT__D[4U]);
    __Vfunc_keccak_f1600__DOT__rotl__2__x = vlSelfRef.keccak_f1600__DOT__Ath[0U];
    keccak_f1600__DOT____VlemCall_2__rotl = __Vfunc_keccak_f1600__DOT__rotl__2__x;
    vlSelfRef.keccak_f1600__DOT__B[0U] = keccak_f1600__DOT____VlemCall_2__rotl;
    __Vfunc_keccak_f1600__DOT__rotl__2__x = vlSelfRef.keccak_f1600__DOT__Ath[5U];
    keccak_f1600__DOT____VlemCall_2__rotl = ((__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                              << 0x00000024U) 
                                             | (__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                                >> 0x0000001cU));
    vlSelfRef.keccak_f1600__DOT__B[16U] = keccak_f1600__DOT____VlemCall_2__rotl;
    __Vfunc_keccak_f1600__DOT__rotl__2__x = vlSelfRef.keccak_f1600__DOT__Ath[10U];
    keccak_f1600__DOT____VlemCall_2__rotl = ((__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                              << 3U) 
                                             | (__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                                >> 0x0000003dU));
    vlSelfRef.keccak_f1600__DOT__B[7U] = keccak_f1600__DOT____VlemCall_2__rotl;
    __Vfunc_keccak_f1600__DOT__rotl__2__x = vlSelfRef.keccak_f1600__DOT__Ath[15U];
    keccak_f1600__DOT____VlemCall_2__rotl = ((__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                              << 0x00000029U) 
                                             | (__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                                >> 0x00000017U));
    vlSelfRef.keccak_f1600__DOT__B[23U] = keccak_f1600__DOT____VlemCall_2__rotl;
    __Vfunc_keccak_f1600__DOT__rotl__2__x = vlSelfRef.keccak_f1600__DOT__Ath[20U];
    keccak_f1600__DOT____VlemCall_2__rotl = ((__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                              << 0x00000012U) 
                                             | (__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                                >> 0x0000002eU));
    vlSelfRef.keccak_f1600__DOT__B[14U] = keccak_f1600__DOT____VlemCall_2__rotl;
    __Vfunc_keccak_f1600__DOT__rotl__2__x = vlSelfRef.keccak_f1600__DOT__Ath[1U];
    keccak_f1600__DOT____VlemCall_2__rotl = ((__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                              << 1U) 
                                             | (__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                                >> 0x0000003fU));
    vlSelfRef.keccak_f1600__DOT__B[10U] = keccak_f1600__DOT____VlemCall_2__rotl;
    __Vfunc_keccak_f1600__DOT__rotl__2__x = vlSelfRef.keccak_f1600__DOT__Ath[6U];
    keccak_f1600__DOT____VlemCall_2__rotl = ((__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                              << 0x0000002cU) 
                                             | (__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                                >> 0x00000014U));
    vlSelfRef.keccak_f1600__DOT__B[1U] = keccak_f1600__DOT____VlemCall_2__rotl;
    __Vfunc_keccak_f1600__DOT__rotl__2__x = vlSelfRef.keccak_f1600__DOT__Ath[11U];
    keccak_f1600__DOT____VlemCall_2__rotl = ((__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                              << 0x0000000aU) 
                                             | (__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                                >> 0x00000036U));
    vlSelfRef.keccak_f1600__DOT__B[17U] = keccak_f1600__DOT____VlemCall_2__rotl;
    __Vfunc_keccak_f1600__DOT__rotl__2__x = vlSelfRef.keccak_f1600__DOT__Ath[16U];
    keccak_f1600__DOT____VlemCall_2__rotl = ((__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                              << 0x0000002dU) 
                                             | (__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                                >> 0x00000013U));
    vlSelfRef.keccak_f1600__DOT__B[8U] = keccak_f1600__DOT____VlemCall_2__rotl;
    __Vfunc_keccak_f1600__DOT__rotl__2__x = vlSelfRef.keccak_f1600__DOT__Ath[21U];
    keccak_f1600__DOT____VlemCall_2__rotl = ((__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                              << 2U) 
                                             | (__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                                >> 0x0000003eU));
    vlSelfRef.keccak_f1600__DOT__B[24U] = keccak_f1600__DOT____VlemCall_2__rotl;
    __Vfunc_keccak_f1600__DOT__rotl__2__x = vlSelfRef.keccak_f1600__DOT__Ath[2U];
    keccak_f1600__DOT____VlemCall_2__rotl = ((__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                              << 0x0000003eU) 
                                             | (__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                                >> 2U));
    vlSelfRef.keccak_f1600__DOT__B[20U] = keccak_f1600__DOT____VlemCall_2__rotl;
    __Vfunc_keccak_f1600__DOT__rotl__2__x = vlSelfRef.keccak_f1600__DOT__Ath[7U];
    keccak_f1600__DOT____VlemCall_2__rotl = ((__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                              << 6U) 
                                             | (__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                                >> 0x0000003aU));
    vlSelfRef.keccak_f1600__DOT__B[11U] = keccak_f1600__DOT____VlemCall_2__rotl;
    __Vfunc_keccak_f1600__DOT__rotl__2__x = vlSelfRef.keccak_f1600__DOT__Ath[12U];
    keccak_f1600__DOT____VlemCall_2__rotl = ((__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                              << 0x0000002bU) 
                                             | (__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                                >> 0x00000015U));
    vlSelfRef.keccak_f1600__DOT__B[2U] = keccak_f1600__DOT____VlemCall_2__rotl;
    __Vfunc_keccak_f1600__DOT__rotl__2__x = vlSelfRef.keccak_f1600__DOT__Ath[17U];
    keccak_f1600__DOT____VlemCall_2__rotl = ((__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                              << 0x0000000fU) 
                                             | (__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                                >> 0x00000031U));
    vlSelfRef.keccak_f1600__DOT__B[18U] = keccak_f1600__DOT____VlemCall_2__rotl;
    __Vfunc_keccak_f1600__DOT__rotl__2__x = vlSelfRef.keccak_f1600__DOT__Ath[22U];
    keccak_f1600__DOT____VlemCall_2__rotl = ((__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                              << 0x0000003dU) 
                                             | (__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                                >> 3U));
    vlSelfRef.keccak_f1600__DOT__B[9U] = keccak_f1600__DOT____VlemCall_2__rotl;
    __Vfunc_keccak_f1600__DOT__rotl__2__x = vlSelfRef.keccak_f1600__DOT__Ath[3U];
    keccak_f1600__DOT____VlemCall_2__rotl = ((__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                              << 0x0000001cU) 
                                             | (__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                                >> 0x00000024U));
    vlSelfRef.keccak_f1600__DOT__B[5U] = keccak_f1600__DOT____VlemCall_2__rotl;
    __Vfunc_keccak_f1600__DOT__rotl__2__x = vlSelfRef.keccak_f1600__DOT__Ath[8U];
    keccak_f1600__DOT____VlemCall_2__rotl = ((__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                              << 0x00000037U) 
                                             | (__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                                >> 9U));
    vlSelfRef.keccak_f1600__DOT__B[21U] = keccak_f1600__DOT____VlemCall_2__rotl;
    __Vfunc_keccak_f1600__DOT__rotl__2__x = vlSelfRef.keccak_f1600__DOT__Ath[13U];
    keccak_f1600__DOT____VlemCall_2__rotl = ((__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                              << 0x00000019U) 
                                             | (__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                                >> 0x00000027U));
    vlSelfRef.keccak_f1600__DOT__B[12U] = keccak_f1600__DOT____VlemCall_2__rotl;
    __Vfunc_keccak_f1600__DOT__rotl__2__x = vlSelfRef.keccak_f1600__DOT__Ath[18U];
    keccak_f1600__DOT____VlemCall_2__rotl = ((__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                              << 0x00000015U) 
                                             | (__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                                >> 0x0000002bU));
    vlSelfRef.keccak_f1600__DOT__B[3U] = keccak_f1600__DOT____VlemCall_2__rotl;
    __Vfunc_keccak_f1600__DOT__rotl__2__x = vlSelfRef.keccak_f1600__DOT__Ath[23U];
    keccak_f1600__DOT____VlemCall_2__rotl = ((__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                              << 0x00000038U) 
                                             | (__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                                >> 8U));
    vlSelfRef.keccak_f1600__DOT__B[19U] = keccak_f1600__DOT____VlemCall_2__rotl;
    __Vfunc_keccak_f1600__DOT__rotl__2__x = vlSelfRef.keccak_f1600__DOT__Ath[4U];
    keccak_f1600__DOT____VlemCall_2__rotl = ((__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                              << 0x0000001bU) 
                                             | (__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                                >> 0x00000025U));
    vlSelfRef.keccak_f1600__DOT__B[15U] = keccak_f1600__DOT____VlemCall_2__rotl;
    __Vfunc_keccak_f1600__DOT__rotl__2__x = vlSelfRef.keccak_f1600__DOT__Ath[9U];
    keccak_f1600__DOT____VlemCall_2__rotl = ((__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                              << 0x00000014U) 
                                             | (__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                                >> 0x0000002cU));
    vlSelfRef.keccak_f1600__DOT__B[6U] = keccak_f1600__DOT____VlemCall_2__rotl;
    __Vfunc_keccak_f1600__DOT__rotl__2__x = vlSelfRef.keccak_f1600__DOT__Ath[14U];
    keccak_f1600__DOT____VlemCall_2__rotl = ((__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                              << 0x00000027U) 
                                             | (__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                                >> 0x00000019U));
    vlSelfRef.keccak_f1600__DOT__B[22U] = keccak_f1600__DOT____VlemCall_2__rotl;
    __Vfunc_keccak_f1600__DOT__rotl__2__x = vlSelfRef.keccak_f1600__DOT__Ath[19U];
    keccak_f1600__DOT____VlemCall_2__rotl = ((__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                              << 8U) 
                                             | (__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                                >> 0x00000038U));
    vlSelfRef.keccak_f1600__DOT__B[13U] = keccak_f1600__DOT____VlemCall_2__rotl;
    __Vfunc_keccak_f1600__DOT__rotl__2__x = vlSelfRef.keccak_f1600__DOT__Ath[24U];
    keccak_f1600__DOT____VlemCall_2__rotl = ((__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                              << 0x0000000eU) 
                                             | (__Vfunc_keccak_f1600__DOT__rotl__2__x 
                                                >> 0x00000032U));
    vlSelfRef.keccak_f1600__DOT__B[4U] = keccak_f1600__DOT____VlemCall_2__rotl;
    vlSelfRef.keccak_f1600__DOT__Anext[0U] = (vlSelfRef.keccak_f1600__DOT__B[0U] 
                                              ^ ((~ vlSelfRef.keccak_f1600__DOT__B[1U]) 
                                                 & vlSelfRef.keccak_f1600__DOT__B[2U]));
    vlSelfRef.keccak_f1600__DOT__Anext[5U] = (vlSelfRef.keccak_f1600__DOT__B[5U] 
                                              ^ ((~ vlSelfRef.keccak_f1600__DOT__B[6U]) 
                                                 & vlSelfRef.keccak_f1600__DOT__B[7U]));
    vlSelfRef.keccak_f1600__DOT__Anext[10U] = (vlSelfRef.keccak_f1600__DOT__B[10U] 
                                               ^ ((~ vlSelfRef.keccak_f1600__DOT__B[11U]) 
                                                  & vlSelfRef.keccak_f1600__DOT__B[12U]));
    vlSelfRef.keccak_f1600__DOT__Anext[15U] = (vlSelfRef.keccak_f1600__DOT__B[15U] 
                                               ^ ((~ vlSelfRef.keccak_f1600__DOT__B[16U]) 
                                                  & vlSelfRef.keccak_f1600__DOT__B[17U]));
    vlSelfRef.keccak_f1600__DOT__Anext[20U] = (vlSelfRef.keccak_f1600__DOT__B[20U] 
                                               ^ ((~ vlSelfRef.keccak_f1600__DOT__B[21U]) 
                                                  & vlSelfRef.keccak_f1600__DOT__B[22U]));
    vlSelfRef.keccak_f1600__DOT__Anext[1U] = (vlSelfRef.keccak_f1600__DOT__B[1U] 
                                              ^ ((~ vlSelfRef.keccak_f1600__DOT__B[2U]) 
                                                 & vlSelfRef.keccak_f1600__DOT__B[3U]));
    vlSelfRef.keccak_f1600__DOT__Anext[6U] = (vlSelfRef.keccak_f1600__DOT__B[6U] 
                                              ^ ((~ vlSelfRef.keccak_f1600__DOT__B[7U]) 
                                                 & vlSelfRef.keccak_f1600__DOT__B[8U]));
    vlSelfRef.keccak_f1600__DOT__Anext[11U] = (vlSelfRef.keccak_f1600__DOT__B[11U] 
                                               ^ ((~ vlSelfRef.keccak_f1600__DOT__B[12U]) 
                                                  & vlSelfRef.keccak_f1600__DOT__B[13U]));
    vlSelfRef.keccak_f1600__DOT__Anext[16U] = (vlSelfRef.keccak_f1600__DOT__B[16U] 
                                               ^ ((~ vlSelfRef.keccak_f1600__DOT__B[17U]) 
                                                  & vlSelfRef.keccak_f1600__DOT__B[18U]));
    vlSelfRef.keccak_f1600__DOT__Anext[21U] = (vlSelfRef.keccak_f1600__DOT__B[21U] 
                                               ^ ((~ vlSelfRef.keccak_f1600__DOT__B[22U]) 
                                                  & vlSelfRef.keccak_f1600__DOT__B[23U]));
    vlSelfRef.keccak_f1600__DOT__Anext[2U] = (vlSelfRef.keccak_f1600__DOT__B[2U] 
                                              ^ ((~ vlSelfRef.keccak_f1600__DOT__B[3U]) 
                                                 & vlSelfRef.keccak_f1600__DOT__B[4U]));
    vlSelfRef.keccak_f1600__DOT__Anext[7U] = (vlSelfRef.keccak_f1600__DOT__B[7U] 
                                              ^ ((~ vlSelfRef.keccak_f1600__DOT__B[8U]) 
                                                 & vlSelfRef.keccak_f1600__DOT__B[9U]));
    vlSelfRef.keccak_f1600__DOT__Anext[12U] = (vlSelfRef.keccak_f1600__DOT__B[12U] 
                                               ^ ((~ vlSelfRef.keccak_f1600__DOT__B[13U]) 
                                                  & vlSelfRef.keccak_f1600__DOT__B[14U]));
    vlSelfRef.keccak_f1600__DOT__Anext[17U] = (vlSelfRef.keccak_f1600__DOT__B[17U] 
                                               ^ ((~ vlSelfRef.keccak_f1600__DOT__B[18U]) 
                                                  & vlSelfRef.keccak_f1600__DOT__B[19U]));
    vlSelfRef.keccak_f1600__DOT__Anext[22U] = (vlSelfRef.keccak_f1600__DOT__B[22U] 
                                               ^ ((~ vlSelfRef.keccak_f1600__DOT__B[23U]) 
                                                  & vlSelfRef.keccak_f1600__DOT__B[24U]));
    vlSelfRef.keccak_f1600__DOT__Anext[3U] = (vlSelfRef.keccak_f1600__DOT__B[3U] 
                                              ^ ((~ vlSelfRef.keccak_f1600__DOT__B[4U]) 
                                                 & vlSelfRef.keccak_f1600__DOT__B[0U]));
    vlSelfRef.keccak_f1600__DOT__Anext[8U] = (vlSelfRef.keccak_f1600__DOT__B[8U] 
                                              ^ ((~ vlSelfRef.keccak_f1600__DOT__B[9U]) 
                                                 & vlSelfRef.keccak_f1600__DOT__B[5U]));
    vlSelfRef.keccak_f1600__DOT__Anext[13U] = (vlSelfRef.keccak_f1600__DOT__B[13U] 
                                               ^ ((~ vlSelfRef.keccak_f1600__DOT__B[14U]) 
                                                  & vlSelfRef.keccak_f1600__DOT__B[10U]));
    vlSelfRef.keccak_f1600__DOT__Anext[18U] = (vlSelfRef.keccak_f1600__DOT__B[18U] 
                                               ^ ((~ vlSelfRef.keccak_f1600__DOT__B[19U]) 
                                                  & vlSelfRef.keccak_f1600__DOT__B[15U]));
    vlSelfRef.keccak_f1600__DOT__Anext[23U] = (vlSelfRef.keccak_f1600__DOT__B[23U] 
                                               ^ ((~ vlSelfRef.keccak_f1600__DOT__B[24U]) 
                                                  & vlSelfRef.keccak_f1600__DOT__B[20U]));
    vlSelfRef.keccak_f1600__DOT__Anext[4U] = (vlSelfRef.keccak_f1600__DOT__B[4U] 
                                              ^ ((~ vlSelfRef.keccak_f1600__DOT__B[0U]) 
                                                 & vlSelfRef.keccak_f1600__DOT__B[1U]));
    vlSelfRef.keccak_f1600__DOT__Anext[9U] = (vlSelfRef.keccak_f1600__DOT__B[9U] 
                                              ^ ((~ vlSelfRef.keccak_f1600__DOT__B[5U]) 
                                                 & vlSelfRef.keccak_f1600__DOT__B[6U]));
    vlSelfRef.keccak_f1600__DOT__Anext[14U] = (vlSelfRef.keccak_f1600__DOT__B[14U] 
                                               ^ ((~ vlSelfRef.keccak_f1600__DOT__B[10U]) 
                                                  & vlSelfRef.keccak_f1600__DOT__B[11U]));
    vlSelfRef.keccak_f1600__DOT__Anext[19U] = (vlSelfRef.keccak_f1600__DOT__B[19U] 
                                               ^ ((~ vlSelfRef.keccak_f1600__DOT__B[15U]) 
                                                  & vlSelfRef.keccak_f1600__DOT__B[16U]));
    vlSelfRef.keccak_f1600__DOT__Anext[24U] = (vlSelfRef.keccak_f1600__DOT__B[24U] 
                                               ^ ((~ vlSelfRef.keccak_f1600__DOT__B[20U]) 
                                                  & vlSelfRef.keccak_f1600__DOT__B[21U]));
    __Vfunc_keccak_f1600__DOT__rc__3__r = vlSelfRef.keccak_f1600__DOT__round_cnt;
    keccak_f1600__DOT____VlemCall_3__rc = (((QData)((IData)(Vkeccak_f1600__ConstPool__CONST_hd522b744_0
                                                            [
                                                            (((IData)(0x0000003fU) 
                                                              + 
                                                              ((IData)(__Vfunc_keccak_f1600__DOT__rc__3__r) 
                                                               << 6U)) 
                                                             >> 5U)])) 
                                            << 0x00000020U) 
                                           | (QData)((IData)(Vkeccak_f1600__ConstPool__CONST_hd522b744_0
                                                             [
                                                             (0x07fffffeU 
                                                              & ((IData)(__Vfunc_keccak_f1600__DOT__rc__3__r) 
                                                                 << 1U))])));
    vlSelfRef.keccak_f1600__DOT__Anext[0U] = (vlSelfRef.keccak_f1600__DOT__Anext[0U] 
                                              ^ keccak_f1600__DOT____VlemCall_3__rc);
}

VL_ATTR_COLD bool Vkeccak_f1600___024root___eval_phase__stl(Vkeccak_f1600___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vkeccak_f1600___024root___eval_phase__stl\n"); );
    Vkeccak_f1600__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    CData/*0:0*/ __VstlExecute;
    // Body
    {
        // Inlined CFunc: _eval_triggers_vec__stl
        vlSelfRef.__VstlTriggered[0U] = ((0xfffffffffffffffeULL 
                                          & vlSelfRef.__VstlTriggered[0U]) 
                                         | (IData)((IData)(vlSelfRef.__VstlFirstIteration)));
    }
#ifdef VL_DEBUG
    if (VL_UNLIKELY(vlSymsp->_vm_contextp__->debug())) {
        Vkeccak_f1600___024root___dump_triggers__stl(vlSelfRef.__VstlTriggered, "stl"s);
    }
#endif
    __VstlExecute = Vkeccak_f1600___024root___trigger_anySet__stl(vlSelfRef.__VstlTriggered);
    if (__VstlExecute) {
        {
            // Inlined CFunc: _eval_stl
            if ((1ULL & vlSelfRef.__VstlTriggered[0U])) {
                Vkeccak_f1600___024root___stl_sequent__TOP__0(vlSelf);
            }
        }
    }
    return (__VstlExecute);
}

bool Vkeccak_f1600___024root___trigger_anySet__ico(const VlUnpacked<QData/*63:0*/, 2> &in);

#ifdef VL_DEBUG
VL_ATTR_COLD void Vkeccak_f1600___024root___dump_triggers__ico(const VlUnpacked<QData/*63:0*/, 2> &triggers, const std::string &tag) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vkeccak_f1600___024root___dump_triggers__ico\n"); );
    // Body
    if ((1U & (~ (IData)(Vkeccak_f1600___024root___trigger_anySet__ico(triggers))))) {
        VL_DBG_MSGS("         No '" + tag + "' region triggers active\n");
    }
    if ((1U & (IData)(triggers[0U]))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 0 is active: @( clk)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 1U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 1 is active: @( rst_n)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 2U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 2 is active: @( start)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 3U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 3 is active: @( wr_en)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 4U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 4 is active: @( wr_addr)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 5U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 5 is active: @( wr_data)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 6U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 6 is active: @( rd_addr)\n");
    }
    if ((1U & (IData)(triggers[1U]))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 64 is active: Internal 'ico' trigger - first iteration\n");
    }
}
#endif  // VL_DEBUG

bool Vkeccak_f1600___024root___trigger_anySet__act(const VlUnpacked<QData/*63:0*/, 1> &in);

#ifdef VL_DEBUG
VL_ATTR_COLD void Vkeccak_f1600___024root___dump_triggers__act(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vkeccak_f1600___024root___dump_triggers__act\n"); );
    // Body
    if ((1U & (~ (IData)(Vkeccak_f1600___024root___trigger_anySet__act(triggers))))) {
        VL_DBG_MSGS("         No '" + tag + "' region triggers active\n");
    }
    if ((1U & (IData)(triggers[0U]))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 0 is active: @(posedge clk)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 1U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 1 is active: @(negedge rst_n)\n");
    }
}
#endif  // VL_DEBUG

VL_ATTR_COLD void Vkeccak_f1600___024root___ctor_var_reset(Vkeccak_f1600___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vkeccak_f1600___024root___ctor_var_reset\n"); );
    Vkeccak_f1600__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    const uint64_t __VscopeHash = VL_MURMUR64_HASH(vlSelf->vlNamep);
    vlSelf->clk = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 16707436170211756652ull);
    vlSelf->rst_n = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 1638864771569018232ull);
    vlSelf->start = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 9867861323841650631ull);
    vlSelf->done = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 10296494685231209730ull);
    vlSelf->wr_en = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 7710928637576349896ull);
    vlSelf->wr_addr = VL_SCOPED_RAND_RESET_I(5, __VscopeHash, 10458723662394441575ull);
    vlSelf->wr_data = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 12812822527505751231ull);
    vlSelf->rd_addr = VL_SCOPED_RAND_RESET_I(5, __VscopeHash, 7950012703377089919ull);
    vlSelf->rd_data = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 17824471296722538975ull);
    for (int __Vi0 = 0; __Vi0 < 25; ++__Vi0) {
        vlSelf->keccak_f1600__DOT__A[__Vi0] = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 13836067283662506613ull);
    }
    vlSelf->keccak_f1600__DOT__round_cnt = VL_SCOPED_RAND_RESET_I(5, __VscopeHash, 493781740944344901ull);
    vlSelf->keccak_f1600__DOT__busy = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 252653877520237517ull);
    for (int __Vi0 = 0; __Vi0 < 5; ++__Vi0) {
        vlSelf->keccak_f1600__DOT__C[__Vi0] = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 6059639175294877708ull);
    }
    for (int __Vi0 = 0; __Vi0 < 5; ++__Vi0) {
        vlSelf->keccak_f1600__DOT__D[__Vi0] = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 15625082952949045820ull);
    }
    for (int __Vi0 = 0; __Vi0 < 25; ++__Vi0) {
        vlSelf->keccak_f1600__DOT__Ath[__Vi0] = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 11140509934737744346ull);
    }
    for (int __Vi0 = 0; __Vi0 < 25; ++__Vi0) {
        vlSelf->keccak_f1600__DOT__B[__Vi0] = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 4651306374935397660ull);
    }
    for (int __Vi0 = 0; __Vi0 < 25; ++__Vi0) {
        vlSelf->keccak_f1600__DOT__Anext[__Vi0] = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 8886521054903759132ull);
    }
    for (int __Vi0 = 0; __Vi0 < 1; ++__Vi0) {
        vlSelf->__VstlTriggered[__Vi0] = 0;
    }
    for (int __Vi0 = 0; __Vi0 < 2; ++__Vi0) {
        vlSelf->__VicoTriggered[__Vi0] = 0;
    }
    vlSelf->__Vtrigprevexpr___TOP__clk__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__rst_n__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__start__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__wr_en__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__wr_addr__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__wr_data__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__rd_addr__0 = 0;
    vlSelf->__VicoDidInit = 0;
    for (int __Vi0 = 0; __Vi0 < 1; ++__Vi0) {
        vlSelf->__VactTriggered[__Vi0] = 0;
    }
    vlSelf->__Vtrigprevexpr___TOP__clk__1 = 0;
    vlSelf->__Vtrigprevexpr___TOP__rst_n__1 = 0;
    for (int __Vi0 = 0; __Vi0 < 1; ++__Vi0) {
        vlSelf->__VnbaTriggered[__Vi0] = 0;
    }
}
