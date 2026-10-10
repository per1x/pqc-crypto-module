// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design internal header
// See Vpqc_accel_axi.h for the primary calling header

#ifndef VERILATED_VPQC_ACCEL_AXI___024ROOT_H_
#define VERILATED_VPQC_ACCEL_AXI___024ROOT_H_  // guard

#include "verilated.h"


class Vpqc_accel_axi__Syms;

class alignas(VL_CACHE_LINE_BYTES) Vpqc_accel_axi___024root final {
  public:

    // DESIGN SPECIFIC STATE
    // Anonymous structures to workaround compiler member-count bugs
    struct {
        VL_IN8(clk,0,0);
        VL_IN8(rst_n,0,0);
        VL_IN8(s_axi_awaddr,7,0);
        VL_IN8(s_axi_awvalid,0,0);
        VL_OUT8(s_axi_awready,0,0);
        VL_IN8(s_axi_wstrb,3,0);
        VL_IN8(s_axi_wvalid,0,0);
        VL_OUT8(s_axi_wready,0,0);
        VL_OUT8(s_axi_bresp,1,0);
        VL_OUT8(s_axi_bvalid,0,0);
        VL_IN8(s_axi_bready,0,0);
        VL_IN8(s_axi_araddr,7,0);
        VL_IN8(s_axi_arvalid,0,0);
        VL_OUT8(s_axi_arready,0,0);
        VL_OUT8(s_axi_rresp,1,0);
        VL_OUT8(s_axi_rvalid,0,0);
        VL_IN8(s_axi_rready,0,0);
        VL_IN8(s_axis_tvalid,0,0);
        VL_OUT8(s_axis_tready,0,0);
        VL_IN8(s_axis_tlast,0,0);
        VL_OUT8(m_axis_tvalid,0,0);
        VL_IN8(m_axis_tready,0,0);
        VL_OUT8(m_axis_tlast,0,0);
        CData/*6:0*/ pqc_accel_axi__DOT__bufa_addr;
        CData/*0:0*/ pqc_accel_axi__DOT__bufa_we;
        CData/*0:0*/ pqc_accel_axi__DOT__done_set;
        CData/*0:0*/ pqc_accel_axi__DOT__err_set;
        CData/*0:0*/ pqc_accel_axi__DOT__out_we;
        CData/*0:0*/ pqc_accel_axi__DOT__busy;
        CData/*0:0*/ pqc_accel_axi__DOT__core_rst_n;
        CData/*3:0*/ pqc_accel_axi__DOT__state;
        CData/*0:0*/ pqc_accel_axi__DOT__is_ntt;
        CData/*0:0*/ pqc_accel_axi__DOT__ntt_start;
        CData/*0:0*/ pqc_accel_axi__DOT__ntt_inverse;
        CData/*0:0*/ pqc_accel_axi__DOT__ntt_wr_en;
        CData/*7:0*/ pqc_accel_axi__DOT__ntt_wr_addr;
        CData/*7:0*/ pqc_accel_axi__DOT__ntt_rd_addr;
        CData/*0:0*/ pqc_accel_axi__DOT__ntt_done;
        CData/*0:0*/ pqc_accel_axi__DOT__kec_start;
        CData/*0:0*/ pqc_accel_axi__DOT__kec_wr_en;
        CData/*4:0*/ pqc_accel_axi__DOT__kec_wr_addr;
        CData/*4:0*/ pqc_accel_axi__DOT__kec_rd_addr;
        CData/*0:0*/ pqc_accel_axi__DOT__shk_start;
        CData/*0:0*/ pqc_accel_axi__DOT__shk_flush;
        CData/*0:0*/ pqc_accel_axi__DOT__shk_zeroize;
        CData/*7:0*/ pqc_accel_axi__DOT__shk_rate;
        CData/*7:0*/ pqc_accel_axi__DOT__shk_suffix;
        CData/*0:0*/ pqc_accel_axi__DOT__shk_in_valid;
        CData/*0:0*/ pqc_accel_axi__DOT__mode_ntt;
        CData/*2:0*/ pqc_accel_axi__DOT__u_ntt__DOT__state;
        CData/*7:0*/ pqc_accel_axi__DOT__u_ntt__DOT__k;
        CData/*0:0*/ pqc_accel_axi__DOT__u_ntt__DOT__inv_r;
        CData/*0:0*/ pqc_accel_axi__DOT__u_ntt__DOT__pa_we;
        CData/*0:0*/ pqc_accel_axi__DOT__u_ntt__DOT__pb_we;
        CData/*7:0*/ pqc_accel_axi__DOT__u_ntt__DOT__pa_addr;
        CData/*7:0*/ pqc_accel_axi__DOT__u_ntt__DOT__pb_addr;
        CData/*3:0*/ pqc_accel_axi__DOT__u_sha3__DOT__state;
        CData/*1:0*/ pqc_accel_axi__DOT__u_sha3__DOT__ret;
        CData/*7:0*/ pqc_accel_axi__DOT__u_sha3__DOT__rate_r;
        CData/*7:0*/ pqc_accel_axi__DOT__u_sha3__DOT__suffix_r;
        CData/*7:0*/ pqc_accel_axi__DOT__u_sha3__DOT__bpos;
        CData/*7:0*/ pqc_accel_axi__DOT__u_sha3__DOT__sq_bpos;
        CData/*4:0*/ pqc_accel_axi__DOT__u_sha3__DOT__clr_idx;
        CData/*0:0*/ pqc_accel_axi__DOT__u_sha3__DOT__kec_start_r;
    };
    struct {
        CData/*0:0*/ pqc_accel_axi__DOT__u_sha3__DOT__kec_start;
        CData/*0:0*/ pqc_accel_axi__DOT__u_sha3__DOT__kec_done;
        CData/*0:0*/ pqc_accel_axi__DOT__u_sha3__DOT__kec_wr_en;
        CData/*4:0*/ pqc_accel_axi__DOT__u_sha3__DOT__kec_wr_addr;
        CData/*0:0*/ pqc_accel_axi__DOT__u_sha3__DOT__perm_busy;
        CData/*0:0*/ pqc_accel_axi__DOT__u_sha3__DOT__zeroize_d;
        CData/*0:0*/ pqc_accel_axi__DOT__u_sha3__DOT__zeroize_rise;
        CData/*4:0*/ pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__round_cnt;
        CData/*0:0*/ pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__busy;
        CData/*0:0*/ pqc_accel_axi__DOT__u_regs__DOT__st_done;
        CData/*0:0*/ pqc_accel_axi__DOT__u_regs__DOT__st_err;
        CData/*0:0*/ pqc_accel_axi__DOT__u_regs__DOT__start_r;
        CData/*0:0*/ pqc_accel_axi__DOT__u_regs__DOT__soft_reset_r;
        CData/*0:0*/ pqc_accel_axi__DOT__u_regs__DOT__aw_hold;
        CData/*0:0*/ pqc_accel_axi__DOT__u_regs__DOT__w_hold;
        CData/*7:0*/ pqc_accel_axi__DOT__u_regs__DOT__aw_addr;
        CData/*3:0*/ pqc_accel_axi__DOT__u_regs__DOT__w_strb;
        CData/*0:0*/ pqc_accel_axi__DOT__u_regs__DOT__aw_go;
        CData/*0:0*/ pqc_accel_axi__DOT__u_regs__DOT__w_go;
        CData/*0:0*/ pqc_accel_axi__DOT__u_regs__DOT__wr_fire;
        CData/*7:0*/ pqc_accel_axi__DOT__u_regs__DOT__wr_addr;
        CData/*3:0*/ pqc_accel_axi__DOT__u_regs__DOT__wr_strb;
        CData/*7:0*/ pqc_accel_axi__DOT__u_regs__DOT__ar_addr;
        CData/*0:0*/ pqc_accel_axi__DOT__u_regs__DOT__ar_hold;
        CData/*3:0*/ __Vdly__pqc_accel_axi__DOT__state;
        CData/*0:0*/ __Vdly__pqc_accel_axi__DOT__is_ntt;
        CData/*0:0*/ __Vdly__pqc_accel_axi__DOT__ntt_start;
        CData/*0:0*/ __Vdly__pqc_accel_axi__DOT__ntt_inverse;
        CData/*0:0*/ __Vdly__pqc_accel_axi__DOT__kec_start;
        CData/*2:0*/ __Vdly__pqc_accel_axi__DOT__u_ntt__DOT__state;
        CData/*3:0*/ __Vdly__pqc_accel_axi__DOT__u_sha3__DOT__state;
        CData/*0:0*/ __VdlySet__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v0;
        CData/*0:0*/ __VdlySet__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v1;
        CData/*0:0*/ __VdlySet__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v25;
        CData/*0:0*/ __VdlySet__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v26;
        CData/*7:0*/ __VdlyDim0__pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem__v0;
        CData/*0:0*/ __VdlySet__pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem__v0;
        CData/*7:0*/ __VdlyDim0__pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem__v1;
        CData/*0:0*/ __VdlySet__pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem__v1;
        CData/*6:0*/ __VdlyDim0__pqc_accel_axi__DOT__u_buf__DOT__mem__v0;
        CData/*0:0*/ __VdlySet__pqc_accel_axi__DOT__u_buf__DOT__mem__v0;
        CData/*0:0*/ __VstlFirstIteration;
        CData/*0:0*/ __VstlPhaseResult;
        CData/*0:0*/ __Vtrigprevexpr___TOP__clk__0;
        CData/*0:0*/ __Vtrigprevexpr___TOP__rst_n__0;
        CData/*7:0*/ __Vtrigprevexpr___TOP__s_axi_awaddr__0;
        CData/*0:0*/ __Vtrigprevexpr___TOP__s_axi_awvalid__0;
        CData/*3:0*/ __Vtrigprevexpr___TOP__s_axi_wstrb__0;
        CData/*0:0*/ __Vtrigprevexpr___TOP__s_axi_wvalid__0;
        CData/*0:0*/ __Vtrigprevexpr___TOP__s_axi_bready__0;
        CData/*7:0*/ __Vtrigprevexpr___TOP__s_axi_araddr__0;
        CData/*0:0*/ __Vtrigprevexpr___TOP__s_axi_arvalid__0;
        CData/*0:0*/ __Vtrigprevexpr___TOP__s_axi_rready__0;
        CData/*0:0*/ __Vtrigprevexpr___TOP__s_axis_tvalid__0;
        CData/*0:0*/ __Vtrigprevexpr___TOP__s_axis_tlast__0;
        CData/*0:0*/ __Vtrigprevexpr___TOP__m_axis_tready__0;
        CData/*0:0*/ __VicoDidInit;
        CData/*0:0*/ __VicoPhaseResult;
        CData/*0:0*/ __Vtrigprevexpr___TOP__clk__1;
        CData/*0:0*/ __Vtrigprevexpr___TOP__rst_n__1;
        CData/*0:0*/ __Vtrigprevexpr___TOP__pqc_accel_axi__DOT__core_rst_n__0;
        CData/*0:0*/ __VactPhaseResult;
        CData/*0:0*/ __VnbaPhaseResult;
        SData/*8:0*/ pqc_accel_axi__DOT__wr_ptr;
    };
    struct {
        SData/*8:0*/ pqc_accel_axi__DOT__rd_ptr;
        SData/*8:0*/ pqc_accel_axi__DOT__out_words;
        SData/*9:0*/ pqc_accel_axi__DOT__cnt;
        SData/*15:0*/ pqc_accel_axi__DOT__lo_lat;
        SData/*15:0*/ pqc_accel_axi__DOT__ntt_wr_data;
        SData/*9:0*/ pqc_accel_axi__DOT__shk_msglen;
        SData/*9:0*/ pqc_accel_axi__DOT__shk_outlen;
        SData/*8:0*/ pqc_accel_axi__DOT__u_ntt__DOT__len;
        SData/*8:0*/ pqc_accel_axi__DOT__u_ntt__DOT__grp;
        SData/*8:0*/ pqc_accel_axi__DOT__u_ntt__DOT__j;
        SData/*8:0*/ pqc_accel_axi__DOT__u_ntt__DOT__scale_i;
        SData/*8:0*/ pqc_accel_axi__DOT__u_ntt__DOT__j_hi;
        SData/*15:0*/ pqc_accel_axi__DOT__u_ntt__DOT__pa_din;
        SData/*15:0*/ pqc_accel_axi__DOT__u_ntt__DOT__pb_din;
        SData/*15:0*/ pqc_accel_axi__DOT__u_ntt__DOT__pa_dout;
        SData/*15:0*/ pqc_accel_axi__DOT__u_ntt__DOT__pb_dout;
        SData/*15:0*/ pqc_accel_axi__DOT__u_ntt__DOT__bf_a_r;
        SData/*15:0*/ pqc_accel_axi__DOT__u_ntt__DOT__gs_a_r;
        SData/*15:0*/ pqc_accel_axi__DOT__u_ntt__DOT__scbarr_r;
        SData/*15:0*/ pqc_accel_axi__DOT__u_ntt__DOT__u_bf_ct_t__DOT__t;
        SData/*15:0*/ pqc_accel_axi__DOT__u_ntt__DOT__u_bf_gs_h__DOT__sum;
        SData/*15:0*/ pqc_accel_axi__DOT__u_ntt__DOT__u_bf_gs_h__DOT__diff;
        SData/*9:0*/ __Vdly__pqc_accel_axi__DOT__cnt;
        SData/*8:0*/ __Vdly__pqc_accel_axi__DOT__rd_ptr;
        SData/*9:0*/ __Vdly__pqc_accel_axi__DOT__shk_msglen;
        SData/*9:0*/ __Vdly__pqc_accel_axi__DOT__shk_outlen;
        SData/*8:0*/ __Vdly__pqc_accel_axi__DOT__u_ntt__DOT__grp;
        SData/*8:0*/ __Vdly__pqc_accel_axi__DOT__u_ntt__DOT__len;
        SData/*15:0*/ __VdlyVal__pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem__v0;
        SData/*15:0*/ __VdlyVal__pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem__v1;
        VL_IN(s_axi_wdata,31,0);
        VL_OUT(s_axi_rdata,31,0);
        VL_IN(s_axis_tdata,31,0);
        VL_OUT(m_axis_tdata,31,0);
        IData/*31:0*/ pqc_accel_axi__DOT__bufa_din;
        IData/*31:0*/ pqc_accel_axi__DOT__bufa_dout;
        IData/*31:0*/ pqc_accel_axi__DOT__bufb_dout;
        IData/*31:0*/ pqc_accel_axi__DOT__out_len_r;
        IData/*31:0*/ pqc_accel_axi__DOT__errcode_r;
        IData/*31:0*/ pqc_accel_axi__DOT__lane_lo;
        IData/*31:0*/ pqc_accel_axi__DOT__sq_acc;
        IData/*31:0*/ pqc_accel_axi__DOT__sq_next;
        IData/*31:0*/ pqc_accel_axi__DOT__u_ntt__DOT__prod_r;
        IData/*31:0*/ pqc_accel_axi__DOT__u_ntt__DOT__scprod_r;
        IData/*31:0*/ pqc_accel_axi__DOT__u_regs__DOT__reg_mode;
        IData/*31:0*/ pqc_accel_axi__DOT__u_regs__DOT__reg_param;
        IData/*31:0*/ pqc_accel_axi__DOT__u_regs__DOT__reg_in_len;
        IData/*31:0*/ pqc_accel_axi__DOT__u_regs__DOT__reg_out_len;
        IData/*31:0*/ pqc_accel_axi__DOT__u_regs__DOT__reg_errcode;
        IData/*31:0*/ pqc_accel_axi__DOT__u_regs__DOT__w_data;
        IData/*31:0*/ pqc_accel_axi__DOT__u_regs__DOT__wr_data;
        IData/*31:0*/ __VdfgRegularize_hebeb780c_0_1;
        IData/*31:0*/ __VdfgRegularize_hebeb780c_0_2;
        IData/*31:0*/ __Vdly__pqc_accel_axi__DOT__lane_lo;
        IData/*31:0*/ __Vdly__pqc_accel_axi__DOT__u_regs__DOT__reg_in_len;
        IData/*31:0*/ __Vdly__pqc_accel_axi__DOT__u_regs__DOT__reg_param;
        IData/*31:0*/ __Vdly__pqc_accel_axi__DOT__u_regs__DOT__reg_mode;
        IData/*31:0*/ __VdlyVal__pqc_accel_axi__DOT__u_buf__DOT__mem__v0;
        IData/*31:0*/ __Vtrigprevexpr___TOP__s_axi_wdata__0;
        IData/*31:0*/ __Vtrigprevexpr___TOP__s_axis_tdata__0;
        IData/*31:0*/ __VactIterCount;
        QData/*63:0*/ pqc_accel_axi__DOT__kec_wr_data;
        QData/*63:0*/ pqc_accel_axi__DOT__kec_rd_data;
        QData/*63:0*/ pqc_accel_axi__DOT__u_sha3__DOT__kec_wr_data;
    };
    struct {
        VlUnpacked<SData/*15:0*/, 128> pqc_accel_axi__DOT__u_ntt__DOT__zetas;
        VlUnpacked<SData/*15:0*/, 256> pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem;
        VlUnpacked<QData/*63:0*/, 25> pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A;
        VlUnpacked<QData/*63:0*/, 5> pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__C;
        VlUnpacked<QData/*63:0*/, 5> pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D;
        VlUnpacked<QData/*63:0*/, 25> pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath;
        VlUnpacked<QData/*63:0*/, 25> pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B;
        VlUnpacked<QData/*63:0*/, 25> pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext;
        VlUnpacked<IData/*31:0*/, 128> pqc_accel_axi__DOT__u_buf__DOT__mem;
        VlUnpacked<QData/*63:0*/, 1> __VstlTriggered;
        VlUnpacked<QData/*63:0*/, 2> __VicoTriggered;
        VlUnpacked<QData/*63:0*/, 1> __VactTriggered;
        VlUnpacked<QData/*63:0*/, 1> __VnbaTriggered;
    };

    // INTERNAL VARIABLES
    Vpqc_accel_axi__Syms* vlSymsp;
    const char* vlNamep;

    // CONSTRUCTORS
    Vpqc_accel_axi___024root(Vpqc_accel_axi__Syms* symsp, const char* namep);
    ~Vpqc_accel_axi___024root();
    VL_UNCOPYABLE(Vpqc_accel_axi___024root);

    // INTERNAL METHODS
    void __Vconfigure(bool first);
};


#endif  // guard
