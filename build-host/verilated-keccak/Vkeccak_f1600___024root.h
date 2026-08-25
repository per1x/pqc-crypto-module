// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design internal header
// See Vkeccak_f1600.h for the primary calling header

#ifndef VERILATED_VKECCAK_F1600___024ROOT_H_
#define VERILATED_VKECCAK_F1600___024ROOT_H_  // guard

#include "verilated.h"


class Vkeccak_f1600__Syms;

class alignas(VL_CACHE_LINE_BYTES) Vkeccak_f1600___024root final {
  public:

    // DESIGN SPECIFIC STATE
    VL_IN8(clk,0,0);
    VL_IN8(rst_n,0,0);
    VL_IN8(start,0,0);
    VL_OUT8(done,0,0);
    VL_IN8(wr_en,0,0);
    VL_IN8(wr_addr,4,0);
    VL_IN8(rd_addr,4,0);
    CData/*4:0*/ keccak_f1600__DOT__round_cnt;
    CData/*0:0*/ keccak_f1600__DOT__busy;
    CData/*0:0*/ __VstlFirstIteration;
    CData/*0:0*/ __VstlPhaseResult;
    CData/*0:0*/ __Vtrigprevexpr___TOP__clk__0;
    CData/*0:0*/ __Vtrigprevexpr___TOP__rst_n__0;
    CData/*0:0*/ __Vtrigprevexpr___TOP__start__0;
    CData/*0:0*/ __Vtrigprevexpr___TOP__wr_en__0;
    CData/*4:0*/ __Vtrigprevexpr___TOP__wr_addr__0;
    CData/*4:0*/ __Vtrigprevexpr___TOP__rd_addr__0;
    CData/*0:0*/ __VicoDidInit;
    CData/*0:0*/ __VicoPhaseResult;
    CData/*0:0*/ __Vtrigprevexpr___TOP__clk__1;
    CData/*0:0*/ __Vtrigprevexpr___TOP__rst_n__1;
    CData/*0:0*/ __VactPhaseResult;
    CData/*0:0*/ __VnbaPhaseResult;
    IData/*31:0*/ __VactIterCount;
    VL_IN64(wr_data,63,0);
    VL_OUT64(rd_data,63,0);
    QData/*63:0*/ __Vtrigprevexpr___TOP__wr_data__0;
    VlUnpacked<QData/*63:0*/, 25> keccak_f1600__DOT__A;
    VlUnpacked<QData/*63:0*/, 5> keccak_f1600__DOT__C;
    VlUnpacked<QData/*63:0*/, 5> keccak_f1600__DOT__D;
    VlUnpacked<QData/*63:0*/, 25> keccak_f1600__DOT__Ath;
    VlUnpacked<QData/*63:0*/, 25> keccak_f1600__DOT__B;
    VlUnpacked<QData/*63:0*/, 25> keccak_f1600__DOT__Anext;
    VlUnpacked<QData/*63:0*/, 1> __VstlTriggered;
    VlUnpacked<QData/*63:0*/, 2> __VicoTriggered;
    VlUnpacked<QData/*63:0*/, 1> __VactTriggered;
    VlUnpacked<QData/*63:0*/, 1> __VnbaTriggered;

    // INTERNAL VARIABLES
    Vkeccak_f1600__Syms* vlSymsp;
    const char* vlNamep;

    // CONSTRUCTORS
    Vkeccak_f1600___024root(Vkeccak_f1600__Syms* symsp, const char* namep);
    ~Vkeccak_f1600___024root();
    VL_UNCOPYABLE(Vkeccak_f1600___024root);

    // INTERNAL METHODS
    void __Vconfigure(bool first);
};


#endif  // guard
