// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design implementation internals
// See Vkeccak_f1600.h for the primary calling header

#include "Vkeccak_f1600__pch.h"

bool Vkeccak_f1600___024root___trigger_anySet__ico(const VlUnpacked<QData/*63:0*/, 2> &in) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vkeccak_f1600___024root___trigger_anySet__ico\n"); );
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

#ifdef VL_DEBUG
VL_ATTR_COLD void Vkeccak_f1600___024root___dump_triggers__ico(const VlUnpacked<QData/*63:0*/, 2> &triggers, const std::string &tag);
#endif  // VL_DEBUG

bool Vkeccak_f1600___024root___eval_phase__ico(Vkeccak_f1600___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vkeccak_f1600___024root___eval_phase__ico\n"); );
    Vkeccak_f1600__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    CData/*0:0*/ __VicoExecute;
    // Body
    {
        // Inlined CFunc: _eval_triggers_vec__ico
        vlSelfRef.__VicoTriggered[0U] = (QData)((IData)(
                                                        (((((IData)(vlSelfRef.rd_addr) 
                                                            != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__rd_addr__0)) 
                                                           << 6U) 
                                                          | (((vlSelfRef.wr_data 
                                                               != vlSelfRef.__Vtrigprevexpr___TOP__wr_data__0) 
                                                              << 5U) 
                                                             | (((IData)(vlSelfRef.wr_addr) 
                                                                 != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__wr_addr__0)) 
                                                                << 4U))) 
                                                         | (((((IData)(vlSelfRef.wr_en) 
                                                               != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__wr_en__0)) 
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
        }
    }
#ifdef VL_DEBUG
    if (VL_UNLIKELY(vlSymsp->_vm_contextp__->debug())) {
        Vkeccak_f1600___024root___dump_triggers__ico(vlSelfRef.__VicoTriggered, "ico"s);
    }
#endif
    __VicoExecute = Vkeccak_f1600___024root___trigger_anySet__ico(vlSelfRef.__VicoTriggered);
    if (__VicoExecute) {
        {
            // Inlined CFunc: _eval_ico
            if ((0x0000000000000040ULL & vlSelfRef.__VicoTriggered[0U])) {
                {
                    // Inlined CFunc: _ico_sequent__TOP__0
                    vlSelfRef.rd_data = (vlSelfRef.keccak_f1600__DOT__A
                                         [vlSelfRef.rd_addr] 
                                         & (- (QData)((IData)(
                                                              (0x18U 
                                                               >= (IData)(vlSelfRef.rd_addr))))));
                }
            }
        }
    }
    return (__VicoExecute);
}

bool Vkeccak_f1600___024root___trigger_anySet__act(const VlUnpacked<QData/*63:0*/, 1> &in) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vkeccak_f1600___024root___trigger_anySet__act\n"); );
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

void Vkeccak_f1600___024root___nba_sequent__TOP__0(Vkeccak_f1600___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vkeccak_f1600___024root___nba_sequent__TOP__0\n"); );
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
    QData/*63:0*/ __VdlyVal__keccak_f1600__DOT__A__v0;
    __VdlyVal__keccak_f1600__DOT__A__v0 = 0;
    CData/*0:0*/ __VdlySet__keccak_f1600__DOT__A__v0;
    __VdlySet__keccak_f1600__DOT__A__v0 = 0;
    QData/*63:0*/ __VdlyVal__keccak_f1600__DOT__A__v1;
    __VdlyVal__keccak_f1600__DOT__A__v1 = 0;
    CData/*0:0*/ __VdlySet__keccak_f1600__DOT__A__v1;
    __VdlySet__keccak_f1600__DOT__A__v1 = 0;
    QData/*63:0*/ __VdlyVal__keccak_f1600__DOT__A__v2;
    __VdlyVal__keccak_f1600__DOT__A__v2 = 0;
    QData/*63:0*/ __VdlyVal__keccak_f1600__DOT__A__v3;
    __VdlyVal__keccak_f1600__DOT__A__v3 = 0;
    QData/*63:0*/ __VdlyVal__keccak_f1600__DOT__A__v4;
    __VdlyVal__keccak_f1600__DOT__A__v4 = 0;
    QData/*63:0*/ __VdlyVal__keccak_f1600__DOT__A__v5;
    __VdlyVal__keccak_f1600__DOT__A__v5 = 0;
    QData/*63:0*/ __VdlyVal__keccak_f1600__DOT__A__v6;
    __VdlyVal__keccak_f1600__DOT__A__v6 = 0;
    QData/*63:0*/ __VdlyVal__keccak_f1600__DOT__A__v7;
    __VdlyVal__keccak_f1600__DOT__A__v7 = 0;
    QData/*63:0*/ __VdlyVal__keccak_f1600__DOT__A__v8;
    __VdlyVal__keccak_f1600__DOT__A__v8 = 0;
    QData/*63:0*/ __VdlyVal__keccak_f1600__DOT__A__v9;
    __VdlyVal__keccak_f1600__DOT__A__v9 = 0;
    QData/*63:0*/ __VdlyVal__keccak_f1600__DOT__A__v10;
    __VdlyVal__keccak_f1600__DOT__A__v10 = 0;
    QData/*63:0*/ __VdlyVal__keccak_f1600__DOT__A__v11;
    __VdlyVal__keccak_f1600__DOT__A__v11 = 0;
    QData/*63:0*/ __VdlyVal__keccak_f1600__DOT__A__v12;
    __VdlyVal__keccak_f1600__DOT__A__v12 = 0;
    QData/*63:0*/ __VdlyVal__keccak_f1600__DOT__A__v13;
    __VdlyVal__keccak_f1600__DOT__A__v13 = 0;
    QData/*63:0*/ __VdlyVal__keccak_f1600__DOT__A__v14;
    __VdlyVal__keccak_f1600__DOT__A__v14 = 0;
    QData/*63:0*/ __VdlyVal__keccak_f1600__DOT__A__v15;
    __VdlyVal__keccak_f1600__DOT__A__v15 = 0;
    QData/*63:0*/ __VdlyVal__keccak_f1600__DOT__A__v16;
    __VdlyVal__keccak_f1600__DOT__A__v16 = 0;
    QData/*63:0*/ __VdlyVal__keccak_f1600__DOT__A__v17;
    __VdlyVal__keccak_f1600__DOT__A__v17 = 0;
    QData/*63:0*/ __VdlyVal__keccak_f1600__DOT__A__v18;
    __VdlyVal__keccak_f1600__DOT__A__v18 = 0;
    QData/*63:0*/ __VdlyVal__keccak_f1600__DOT__A__v19;
    __VdlyVal__keccak_f1600__DOT__A__v19 = 0;
    QData/*63:0*/ __VdlyVal__keccak_f1600__DOT__A__v20;
    __VdlyVal__keccak_f1600__DOT__A__v20 = 0;
    QData/*63:0*/ __VdlyVal__keccak_f1600__DOT__A__v21;
    __VdlyVal__keccak_f1600__DOT__A__v21 = 0;
    QData/*63:0*/ __VdlyVal__keccak_f1600__DOT__A__v22;
    __VdlyVal__keccak_f1600__DOT__A__v22 = 0;
    QData/*63:0*/ __VdlyVal__keccak_f1600__DOT__A__v23;
    __VdlyVal__keccak_f1600__DOT__A__v23 = 0;
    QData/*63:0*/ __VdlyVal__keccak_f1600__DOT__A__v24;
    __VdlyVal__keccak_f1600__DOT__A__v24 = 0;
    QData/*63:0*/ __VdlyVal__keccak_f1600__DOT__A__v25;
    __VdlyVal__keccak_f1600__DOT__A__v25 = 0;
    CData/*4:0*/ __VdlyDim0__keccak_f1600__DOT__A__v25;
    __VdlyDim0__keccak_f1600__DOT__A__v25 = 0;
    CData/*0:0*/ __VdlySet__keccak_f1600__DOT__A__v25;
    __VdlySet__keccak_f1600__DOT__A__v25 = 0;
    CData/*0:0*/ __VdlySet__keccak_f1600__DOT__A__v26;
    __VdlySet__keccak_f1600__DOT__A__v26 = 0;
    // Body
    __VdlySet__keccak_f1600__DOT__A__v0 = 0U;
    __VdlySet__keccak_f1600__DOT__A__v1 = 0U;
    __VdlySet__keccak_f1600__DOT__A__v25 = 0U;
    __VdlySet__keccak_f1600__DOT__A__v26 = 0U;
    if (vlSelfRef.rst_n) {
        if (vlSelfRef.keccak_f1600__DOT__busy) {
            __VdlyVal__keccak_f1600__DOT__A__v0 = vlSelfRef.keccak_f1600__DOT__Anext[0U];
            __VdlySet__keccak_f1600__DOT__A__v0 = 1U;
            if ((0x17U == (IData)(vlSelfRef.keccak_f1600__DOT__round_cnt))) {
                vlSelfRef.keccak_f1600__DOT__busy = 0U;
                vlSelfRef.done = 1U;
            } else {
                vlSelfRef.keccak_f1600__DOT__round_cnt 
                    = (0x0000001fU & ((IData)(1U) + (IData)(vlSelfRef.keccak_f1600__DOT__round_cnt)));
            }
            __VdlyVal__keccak_f1600__DOT__A__v1 = vlSelfRef.keccak_f1600__DOT__Anext[1U];
            __VdlySet__keccak_f1600__DOT__A__v1 = 1U;
            __VdlyVal__keccak_f1600__DOT__A__v2 = vlSelfRef.keccak_f1600__DOT__Anext[2U];
            __VdlyVal__keccak_f1600__DOT__A__v3 = vlSelfRef.keccak_f1600__DOT__Anext[3U];
            __VdlyVal__keccak_f1600__DOT__A__v4 = vlSelfRef.keccak_f1600__DOT__Anext[4U];
            __VdlyVal__keccak_f1600__DOT__A__v5 = vlSelfRef.keccak_f1600__DOT__Anext[5U];
            __VdlyVal__keccak_f1600__DOT__A__v6 = vlSelfRef.keccak_f1600__DOT__Anext[6U];
            __VdlyVal__keccak_f1600__DOT__A__v7 = vlSelfRef.keccak_f1600__DOT__Anext[7U];
            __VdlyVal__keccak_f1600__DOT__A__v8 = vlSelfRef.keccak_f1600__DOT__Anext[8U];
            __VdlyVal__keccak_f1600__DOT__A__v9 = vlSelfRef.keccak_f1600__DOT__Anext[9U];
            __VdlyVal__keccak_f1600__DOT__A__v10 = vlSelfRef.keccak_f1600__DOT__Anext[10U];
            __VdlyVal__keccak_f1600__DOT__A__v11 = vlSelfRef.keccak_f1600__DOT__Anext[11U];
            __VdlyVal__keccak_f1600__DOT__A__v12 = vlSelfRef.keccak_f1600__DOT__Anext[12U];
            __VdlyVal__keccak_f1600__DOT__A__v13 = vlSelfRef.keccak_f1600__DOT__Anext[13U];
            __VdlyVal__keccak_f1600__DOT__A__v14 = vlSelfRef.keccak_f1600__DOT__Anext[14U];
            __VdlyVal__keccak_f1600__DOT__A__v15 = vlSelfRef.keccak_f1600__DOT__Anext[15U];
            __VdlyVal__keccak_f1600__DOT__A__v16 = vlSelfRef.keccak_f1600__DOT__Anext[16U];
            __VdlyVal__keccak_f1600__DOT__A__v17 = vlSelfRef.keccak_f1600__DOT__Anext[17U];
            __VdlyVal__keccak_f1600__DOT__A__v18 = vlSelfRef.keccak_f1600__DOT__Anext[18U];
            __VdlyVal__keccak_f1600__DOT__A__v19 = vlSelfRef.keccak_f1600__DOT__Anext[19U];
            __VdlyVal__keccak_f1600__DOT__A__v20 = vlSelfRef.keccak_f1600__DOT__Anext[20U];
            __VdlyVal__keccak_f1600__DOT__A__v21 = vlSelfRef.keccak_f1600__DOT__Anext[21U];
            __VdlyVal__keccak_f1600__DOT__A__v22 = vlSelfRef.keccak_f1600__DOT__Anext[22U];
            __VdlyVal__keccak_f1600__DOT__A__v23 = vlSelfRef.keccak_f1600__DOT__Anext[23U];
            __VdlyVal__keccak_f1600__DOT__A__v24 = vlSelfRef.keccak_f1600__DOT__Anext[24U];
        } else {
            if (vlSelfRef.wr_en) {
                if ((0x18U >= (IData)(vlSelfRef.wr_addr))) {
                    __VdlyVal__keccak_f1600__DOT__A__v25 
                        = vlSelfRef.wr_data;
                    __VdlyDim0__keccak_f1600__DOT__A__v25 
                        = vlSelfRef.wr_addr;
                    __VdlySet__keccak_f1600__DOT__A__v25 = 1U;
                }
            }
            if (vlSelfRef.start) {
                vlSelfRef.keccak_f1600__DOT__round_cnt = 0U;
                vlSelfRef.done = 0U;
                vlSelfRef.keccak_f1600__DOT__busy = 1U;
            }
        }
    } else {
        vlSelfRef.keccak_f1600__DOT__round_cnt = 0U;
        vlSelfRef.done = 0U;
        vlSelfRef.keccak_f1600__DOT__busy = 0U;
        __VdlySet__keccak_f1600__DOT__A__v26 = 1U;
    }
    if (__VdlySet__keccak_f1600__DOT__A__v0) {
        vlSelfRef.keccak_f1600__DOT__A[0U] = __VdlyVal__keccak_f1600__DOT__A__v0;
    }
    if (__VdlySet__keccak_f1600__DOT__A__v1) {
        vlSelfRef.keccak_f1600__DOT__A[1U] = __VdlyVal__keccak_f1600__DOT__A__v1;
        vlSelfRef.keccak_f1600__DOT__A[2U] = __VdlyVal__keccak_f1600__DOT__A__v2;
        vlSelfRef.keccak_f1600__DOT__A[3U] = __VdlyVal__keccak_f1600__DOT__A__v3;
        vlSelfRef.keccak_f1600__DOT__A[4U] = __VdlyVal__keccak_f1600__DOT__A__v4;
        vlSelfRef.keccak_f1600__DOT__A[5U] = __VdlyVal__keccak_f1600__DOT__A__v5;
        vlSelfRef.keccak_f1600__DOT__A[6U] = __VdlyVal__keccak_f1600__DOT__A__v6;
        vlSelfRef.keccak_f1600__DOT__A[7U] = __VdlyVal__keccak_f1600__DOT__A__v7;
        vlSelfRef.keccak_f1600__DOT__A[8U] = __VdlyVal__keccak_f1600__DOT__A__v8;
        vlSelfRef.keccak_f1600__DOT__A[9U] = __VdlyVal__keccak_f1600__DOT__A__v9;
        vlSelfRef.keccak_f1600__DOT__A[10U] = __VdlyVal__keccak_f1600__DOT__A__v10;
        vlSelfRef.keccak_f1600__DOT__A[11U] = __VdlyVal__keccak_f1600__DOT__A__v11;
        vlSelfRef.keccak_f1600__DOT__A[12U] = __VdlyVal__keccak_f1600__DOT__A__v12;
        vlSelfRef.keccak_f1600__DOT__A[13U] = __VdlyVal__keccak_f1600__DOT__A__v13;
        vlSelfRef.keccak_f1600__DOT__A[14U] = __VdlyVal__keccak_f1600__DOT__A__v14;
        vlSelfRef.keccak_f1600__DOT__A[15U] = __VdlyVal__keccak_f1600__DOT__A__v15;
        vlSelfRef.keccak_f1600__DOT__A[16U] = __VdlyVal__keccak_f1600__DOT__A__v16;
        vlSelfRef.keccak_f1600__DOT__A[17U] = __VdlyVal__keccak_f1600__DOT__A__v17;
        vlSelfRef.keccak_f1600__DOT__A[18U] = __VdlyVal__keccak_f1600__DOT__A__v18;
        vlSelfRef.keccak_f1600__DOT__A[19U] = __VdlyVal__keccak_f1600__DOT__A__v19;
        vlSelfRef.keccak_f1600__DOT__A[20U] = __VdlyVal__keccak_f1600__DOT__A__v20;
        vlSelfRef.keccak_f1600__DOT__A[21U] = __VdlyVal__keccak_f1600__DOT__A__v21;
        vlSelfRef.keccak_f1600__DOT__A[22U] = __VdlyVal__keccak_f1600__DOT__A__v22;
        vlSelfRef.keccak_f1600__DOT__A[23U] = __VdlyVal__keccak_f1600__DOT__A__v23;
        vlSelfRef.keccak_f1600__DOT__A[24U] = __VdlyVal__keccak_f1600__DOT__A__v24;
    }
    if (__VdlySet__keccak_f1600__DOT__A__v25) {
        vlSelfRef.keccak_f1600__DOT__A[__VdlyDim0__keccak_f1600__DOT__A__v25] 
            = __VdlyVal__keccak_f1600__DOT__A__v25;
    }
    if (__VdlySet__keccak_f1600__DOT__A__v26) {
        vlSelfRef.keccak_f1600__DOT__A[0U] = 0ULL;
        vlSelfRef.keccak_f1600__DOT__A[1U] = 0ULL;
        vlSelfRef.keccak_f1600__DOT__A[2U] = 0ULL;
        vlSelfRef.keccak_f1600__DOT__A[3U] = 0ULL;
        vlSelfRef.keccak_f1600__DOT__A[4U] = 0ULL;
        vlSelfRef.keccak_f1600__DOT__A[5U] = 0ULL;
        vlSelfRef.keccak_f1600__DOT__A[6U] = 0ULL;
        vlSelfRef.keccak_f1600__DOT__A[7U] = 0ULL;
        vlSelfRef.keccak_f1600__DOT__A[8U] = 0ULL;
        vlSelfRef.keccak_f1600__DOT__A[9U] = 0ULL;
        vlSelfRef.keccak_f1600__DOT__A[10U] = 0ULL;
        vlSelfRef.keccak_f1600__DOT__A[11U] = 0ULL;
        vlSelfRef.keccak_f1600__DOT__A[12U] = 0ULL;
        vlSelfRef.keccak_f1600__DOT__A[13U] = 0ULL;
        vlSelfRef.keccak_f1600__DOT__A[14U] = 0ULL;
        vlSelfRef.keccak_f1600__DOT__A[15U] = 0ULL;
        vlSelfRef.keccak_f1600__DOT__A[16U] = 0ULL;
        vlSelfRef.keccak_f1600__DOT__A[17U] = 0ULL;
        vlSelfRef.keccak_f1600__DOT__A[18U] = 0ULL;
        vlSelfRef.keccak_f1600__DOT__A[19U] = 0ULL;
        vlSelfRef.keccak_f1600__DOT__A[20U] = 0ULL;
        vlSelfRef.keccak_f1600__DOT__A[21U] = 0ULL;
        vlSelfRef.keccak_f1600__DOT__A[22U] = 0ULL;
        vlSelfRef.keccak_f1600__DOT__A[23U] = 0ULL;
        vlSelfRef.keccak_f1600__DOT__A[24U] = 0ULL;
    }
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

void Vkeccak_f1600___024root___trigger_orInto__act_vec_vec(VlUnpacked<QData/*63:0*/, 1> &out, const VlUnpacked<QData/*63:0*/, 1> &in) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vkeccak_f1600___024root___trigger_orInto__act_vec_vec\n"); );
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
VL_ATTR_COLD void Vkeccak_f1600___024root___dump_triggers__act(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag);
#endif  // VL_DEBUG

bool Vkeccak_f1600___024root___eval_phase__act(Vkeccak_f1600___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vkeccak_f1600___024root___eval_phase__act\n"); );
    Vkeccak_f1600__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
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
        Vkeccak_f1600___024root___dump_triggers__act(vlSelfRef.__VactTriggered, "act"s);
    }
#endif
    Vkeccak_f1600___024root___trigger_orInto__act_vec_vec(vlSelfRef.__VnbaTriggered, vlSelfRef.__VactTriggered);
    return (0U);
}

void Vkeccak_f1600___024root___trigger_clear__act(VlUnpacked<QData/*63:0*/, 1> &out) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vkeccak_f1600___024root___trigger_clear__act\n"); );
    // Locals
    IData/*31:0*/ n;
    // Body
    n = 0U;
    do {
        out[n] = 0ULL;
        n = ((IData)(1U) + n);
    } while ((1U > n));
}

bool Vkeccak_f1600___024root___eval_phase__nba(Vkeccak_f1600___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vkeccak_f1600___024root___eval_phase__nba\n"); );
    Vkeccak_f1600__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    CData/*0:0*/ __VnbaExecute;
    // Body
    __VnbaExecute = Vkeccak_f1600___024root___trigger_anySet__act(vlSelfRef.__VnbaTriggered);
    if (__VnbaExecute) {
        {
            // Inlined CFunc: _eval_nba
            if ((3ULL & vlSelfRef.__VnbaTriggered[0U])) {
                Vkeccak_f1600___024root___nba_sequent__TOP__0(vlSelf);
            }
        }
        Vkeccak_f1600___024root___trigger_clear__act(vlSelfRef.__VnbaTriggered);
    }
    return (__VnbaExecute);
}

void Vkeccak_f1600___024root___eval(Vkeccak_f1600___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vkeccak_f1600___024root___eval\n"); );
    Vkeccak_f1600__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    IData/*31:0*/ __VicoIterCount;
    IData/*31:0*/ __VnbaIterCount;
    // Body
    __VicoIterCount = 0U;
    do {
        if (VL_UNLIKELY(((0x00002710U < __VicoIterCount)))) {
#ifdef VL_DEBUG
            Vkeccak_f1600___024root___dump_triggers__ico(vlSelfRef.__VicoTriggered, "ico"s);
#endif
            VL_FATAL_MT("/Volumes/Work/projects/code/pqc-hsm-fpga/hardware/rtl/keccak/keccak_f1600.v", 20, "", "DIDNOTCONVERGE: Input combinational region did not converge after '--converge-limit' of 10000 tries");
        }
        __VicoIterCount = ((IData)(1U) + __VicoIterCount);
        vlSelfRef.__VicoPhaseResult = Vkeccak_f1600___024root___eval_phase__ico(vlSelf);
    } while (vlSelfRef.__VicoPhaseResult);
    __VnbaIterCount = 0U;
    do {
        if (VL_UNLIKELY(((0x00002710U < __VnbaIterCount)))) {
#ifdef VL_DEBUG
            Vkeccak_f1600___024root___dump_triggers__act(vlSelfRef.__VnbaTriggered, "nba"s);
#endif
            VL_FATAL_MT("/Volumes/Work/projects/code/pqc-hsm-fpga/hardware/rtl/keccak/keccak_f1600.v", 20, "", "DIDNOTCONVERGE: NBA region did not converge after '--converge-limit' of 10000 tries");
        }
        __VnbaIterCount = ((IData)(1U) + __VnbaIterCount);
        vlSelfRef.__VactIterCount = 0U;
        do {
            if (VL_UNLIKELY(((0x00002710U < vlSelfRef.__VactIterCount)))) {
#ifdef VL_DEBUG
                Vkeccak_f1600___024root___dump_triggers__act(vlSelfRef.__VactTriggered, "act"s);
#endif
                VL_FATAL_MT("/Volumes/Work/projects/code/pqc-hsm-fpga/hardware/rtl/keccak/keccak_f1600.v", 20, "", "DIDNOTCONVERGE: Active region did not converge after '--converge-limit' of 10000 tries");
            }
            vlSelfRef.__VactIterCount = ((IData)(1U) 
                                         + vlSelfRef.__VactIterCount);
            vlSelfRef.__VactPhaseResult = Vkeccak_f1600___024root___eval_phase__act(vlSelf);
        } while (vlSelfRef.__VactPhaseResult);
        vlSelfRef.__VnbaPhaseResult = Vkeccak_f1600___024root___eval_phase__nba(vlSelf);
    } while (vlSelfRef.__VnbaPhaseResult);
}

#ifdef VL_DEBUG
void Vkeccak_f1600___024root___eval_debug_assertions(Vkeccak_f1600___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vkeccak_f1600___024root___eval_debug_assertions\n"); );
    Vkeccak_f1600__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
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
    if (VL_UNLIKELY(((vlSelfRef.wr_en & 0xfeU)))) {
        Verilated::overWidthError("wr_en");
    }
    if (VL_UNLIKELY(((vlSelfRef.wr_addr & 0xe0U)))) {
        Verilated::overWidthError("wr_addr");
    }
    if (VL_UNLIKELY(((vlSelfRef.rd_addr & 0xe0U)))) {
        Verilated::overWidthError("rd_addr");
    }
}
#endif  // VL_DEBUG
