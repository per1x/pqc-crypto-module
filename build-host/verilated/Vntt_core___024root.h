// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design internal header
// See Vntt_core.h for the primary calling header

#ifndef VERILATED_VNTT_CORE___024ROOT_H_
#define VERILATED_VNTT_CORE___024ROOT_H_  // guard

#include "verilated.h"


class Vntt_core__Syms;

class alignas(VL_CACHE_LINE_BYTES) Vntt_core___024root final {
  public:

    // DESIGN SPECIFIC STATE
    // Anonymous structures to workaround compiler member-count bugs
    struct {
        VL_IN8(clk,0,0);
        VL_IN8(rst_n,0,0);
        VL_IN8(start,0,0);
        VL_IN8(inverse,0,0);
        VL_OUT8(done,0,0);
        VL_IN8(wr_en,0,0);
        VL_IN8(wr_addr,7,0);
        VL_IN8(rd_addr,7,0);
        CData/*2:0*/ ntt_core__DOT__state;
        CData/*7:0*/ ntt_core__DOT__k;
        CData/*0:0*/ ntt_core__DOT__inv_r;
        CData/*0:0*/ ntt_core__DOT__pa_we;
        CData/*0:0*/ ntt_core__DOT__pb_we;
        CData/*7:0*/ ntt_core__DOT__pa_addr;
        CData/*7:0*/ ntt_core__DOT__pb_addr;
        CData/*7:0*/ __VdlyDim0__ntt_core__DOT__u_mem__DOT__mem__v0;
        CData/*0:0*/ __VdlySet__ntt_core__DOT__u_mem__DOT__mem__v0;
        CData/*7:0*/ __VdlyDim0__ntt_core__DOT__u_mem__DOT__mem__v1;
        CData/*0:0*/ __VdlySet__ntt_core__DOT__u_mem__DOT__mem__v1;
        CData/*0:0*/ __VstlFirstIteration;
        CData/*0:0*/ __VstlPhaseResult;
        CData/*0:0*/ __Vtrigprevexpr___TOP__clk__0;
        CData/*0:0*/ __Vtrigprevexpr___TOP__rst_n__0;
        CData/*0:0*/ __Vtrigprevexpr___TOP__start__0;
        CData/*0:0*/ __Vtrigprevexpr___TOP__inverse__0;
        CData/*0:0*/ __Vtrigprevexpr___TOP__wr_en__0;
        CData/*7:0*/ __Vtrigprevexpr___TOP__wr_addr__0;
        CData/*7:0*/ __Vtrigprevexpr___TOP__rd_addr__0;
        CData/*0:0*/ __VicoDidInit;
        CData/*0:0*/ __VicoPhaseResult;
        CData/*0:0*/ __Vtrigprevexpr___TOP__clk__1;
        CData/*0:0*/ __Vtrigprevexpr___TOP__rst_n__1;
        CData/*0:0*/ __VactPhaseResult;
        CData/*0:0*/ __VnbaPhaseResult;
        VL_IN16(wr_data,15,0);
        VL_OUT16(rd_data,15,0);
        SData/*8:0*/ ntt_core__DOT__len;
        SData/*8:0*/ ntt_core__DOT__grp;
        SData/*8:0*/ ntt_core__DOT__j;
        SData/*8:0*/ ntt_core__DOT__scale_i;
        SData/*8:0*/ ntt_core__DOT__j_hi;
        SData/*15:0*/ ntt_core__DOT__pa_din;
        SData/*15:0*/ ntt_core__DOT__pb_din;
        SData/*15:0*/ ntt_core__DOT__pa_dout;
        SData/*15:0*/ ntt_core__DOT__pb_dout;
        SData/*15:0*/ ntt_core__DOT__bf_a_r;
        SData/*15:0*/ ntt_core__DOT__gs_a_r;
        SData/*15:0*/ ntt_core__DOT__scbarr_r;
        SData/*15:0*/ ntt_core__DOT__u_bf_ct_t__DOT__t;
        SData/*15:0*/ ntt_core__DOT__u_bf_gs_h__DOT__sum;
        SData/*15:0*/ ntt_core__DOT__u_bf_gs_h__DOT__diff;
        SData/*15:0*/ __VdlyVal__ntt_core__DOT__u_mem__DOT__mem__v0;
        SData/*15:0*/ __VdlyVal__ntt_core__DOT__u_mem__DOT__mem__v1;
        SData/*15:0*/ __Vtrigprevexpr___TOP__wr_data__0;
        IData/*31:0*/ ntt_core__DOT__prod_r;
        IData/*31:0*/ ntt_core__DOT__scprod_r;
        IData/*31:0*/ __VdfgRegularize_hebeb780c_0_0;
        IData/*31:0*/ __VdfgRegularize_hebeb780c_0_1;
        IData/*31:0*/ __VactIterCount;
        VlUnpacked<SData/*15:0*/, 128> ntt_core__DOT__zetas;
        VlUnpacked<SData/*15:0*/, 256> ntt_core__DOT__u_mem__DOT__mem;
        VlUnpacked<QData/*63:0*/, 1> __VstlTriggered;
        VlUnpacked<QData/*63:0*/, 2> __VicoTriggered;
        VlUnpacked<QData/*63:0*/, 1> __VactTriggered;
    };
    struct {
        VlUnpacked<QData/*63:0*/, 1> __VnbaTriggered;
    };

    // INTERNAL VARIABLES
    Vntt_core__Syms* vlSymsp;
    const char* vlNamep;

    // CONSTRUCTORS
    Vntt_core___024root(Vntt_core__Syms* symsp, const char* namep);
    ~Vntt_core___024root();
    VL_UNCOPYABLE(Vntt_core___024root);

    // INTERNAL METHODS
    void __Vconfigure(bool first);
};


#endif  // guard
