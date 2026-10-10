// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design implementation internals
// See Vpqc_accel_axi.h for the primary calling header

#include "Vpqc_accel_axi__pch.h"

VL_ATTR_COLD void Vpqc_accel_axi___024root___eval_static(Vpqc_accel_axi___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vpqc_accel_axi___024root___eval_static\n"); );
    Vpqc_accel_axi__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
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
    vlSelfRef.__Vtrigprevexpr___TOP__clk__1 = vlSelfRef.clk;
    vlSelfRef.__Vtrigprevexpr___TOP__rst_n__1 = vlSelfRef.rst_n;
    vlSelfRef.__Vtrigprevexpr___TOP__pqc_accel_axi__DOT__core_rst_n__0 
        = vlSelfRef.pqc_accel_axi__DOT__core_rst_n;
}

VL_ATTR_COLD void Vpqc_accel_axi___024root___eval_initial__TOP(Vpqc_accel_axi___024root* vlSelf);

VL_ATTR_COLD void Vpqc_accel_axi___024root___eval_initial(Vpqc_accel_axi___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vpqc_accel_axi___024root___eval_initial\n"); );
    Vpqc_accel_axi__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    Vpqc_accel_axi___024root___eval_initial__TOP(vlSelf);
}

VL_ATTR_COLD void Vpqc_accel_axi___024root___eval_initial__TOP(Vpqc_accel_axi___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vpqc_accel_axi___024root___eval_initial__TOP\n"); );
    Vpqc_accel_axi__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    IData/*31:0*/ pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__i;
    pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__i = 0;
    IData/*31:0*/ pqc_accel_axi__DOT__u_buf__DOT__i;
    pqc_accel_axi__DOT__u_buf__DOT__i = 0;
    // Body
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[0U] = 0xfbecU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[1U] = 0xfd0aU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[2U] = 0xfe99U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[3U] = 0xfa13U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[4U] = 0x05d5U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[5U] = 0x058eU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[6U] = 0x011fU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[7U] = 0x00caU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[8U] = 0xff55U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[9U] = 0x026eU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[10U] = 0x0629U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[11U] = 0x00b6U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[12U] = 0x03c2U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[13U] = 0xfb4eU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[14U] = 0xfa3eU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[15U] = 0x05bcU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[16U] = 0x023dU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[17U] = 0xfad3U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[18U] = 0x0108U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[19U] = 0x017fU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[20U] = 0xfcc3U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[21U] = 0x05b2U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[22U] = 0xf9beU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[23U] = 0xff7eU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[24U] = 0xfd57U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[25U] = 0x03f9U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[26U] = 0x02dcU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[27U] = 0x0260U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[28U] = 0xf9faU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[29U] = 0x019bU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[30U] = 0xff33U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[31U] = 0xf9ddU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[32U] = 0x04c7U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[33U] = 0x028cU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[34U] = 0xfdd8U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[35U] = 0x03f7U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[36U] = 0xfaf3U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[37U] = 0x05d3U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[38U] = 0xfee6U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[39U] = 0xf9f8U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[40U] = 0x0204U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[41U] = 0xfff8U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[42U] = 0xfec0U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[43U] = 0xfd66U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[44U] = 0xf9aeU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[45U] = 0xfb76U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[46U] = 0x007eU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[47U] = 0x05bdU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[48U] = 0xfcabU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[49U] = 0xffa6U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[50U] = 0xfef1U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[51U] = 0x033eU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[52U] = 0x006bU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[53U] = 0xfa73U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[54U] = 0xff09U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[55U] = 0xfc49U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[56U] = 0xfe72U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[57U] = 0x03c1U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[58U] = 0xfa1cU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[59U] = 0xfd2bU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[60U] = 0x01c0U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[61U] = 0xfbd7U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[62U] = 0x02a5U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[63U] = 0xfb05U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[64U] = 0xfbb1U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[65U] = 0x01aeU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[66U] = 0x022bU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[67U] = 0x034bU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[68U] = 0xfb1dU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[69U] = 0x0367U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[70U] = 0x060eU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[71U] = 0x0069U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[72U] = 0x01a6U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[73U] = 0x024bU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[74U] = 0x00b1U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[75U] = 0xff15U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[76U] = 0xfeddU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[77U] = 0xfe34U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[78U] = 0x0626U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[79U] = 0x0675U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[80U] = 0xff0aU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[81U] = 0x030aU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[82U] = 0x0487U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[83U] = 0xff6dU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[84U] = 0xfcf7U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[85U] = 0x05cbU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[86U] = 0xfda6U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[87U] = 0x045fU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[88U] = 0xf9caU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[89U] = 0x0284U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[90U] = 0xfc98U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[91U] = 0x015dU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[92U] = 0x01a2U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[93U] = 0x0149U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[94U] = 0xff64U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[95U] = 0xffb5U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[96U] = 0x0331U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[97U] = 0x0449U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[98U] = 0x025bU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[99U] = 0x0262U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[100U] = 0x052aU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[101U] = 0xfafbU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[102U] = 0xfa47U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[103U] = 0x0180U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[104U] = 0xfb41U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[105U] = 0xff78U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[106U] = 0x04c2U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[107U] = 0xfac9U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[108U] = 0xfc96U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[109U] = 0x00dcU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[110U] = 0xfb5dU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[111U] = 0xf985U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[112U] = 0xfb5fU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[113U] = 0xfa06U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[114U] = 0xfb02U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[115U] = 0x031aU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[116U] = 0xfa1aU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[117U] = 0xfcaaU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[118U] = 0xfc9aU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[119U] = 0x01deU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[120U] = 0xff94U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[121U] = 0xfeccU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[122U] = 0x03e4U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[123U] = 0x03dfU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[124U] = 0x03beU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[125U] = 0xfa4cU;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[126U] = 0x05f2U;
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__zetas[127U] = 0x065cU;
    pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__i = 0U;
    while (VL_GTS_III(32, 0x00000100U, pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__i)) {
        vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem[(0x000000ffU 
                                                                   & pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__i)] = 0U;
        pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__i 
            = ((IData)(1U) + pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__i);
    }
    pqc_accel_axi__DOT__u_buf__DOT__i = 0U;
    while (VL_GTS_III(32, 0x00000080U, pqc_accel_axi__DOT__u_buf__DOT__i)) {
        vlSelfRef.pqc_accel_axi__DOT__u_buf__DOT__mem[(0x0000007fU 
                                                       & pqc_accel_axi__DOT__u_buf__DOT__i)] = 0U;
        pqc_accel_axi__DOT__u_buf__DOT__i = ((IData)(1U) 
                                             + pqc_accel_axi__DOT__u_buf__DOT__i);
    }
}

VL_ATTR_COLD void Vpqc_accel_axi___024root___eval_final(Vpqc_accel_axi___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vpqc_accel_axi___024root___eval_final\n"); );
    Vpqc_accel_axi__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Vpqc_accel_axi___024root___dump_triggers__stl(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag);
#endif  // VL_DEBUG
VL_ATTR_COLD bool Vpqc_accel_axi___024root___eval_phase__stl(Vpqc_accel_axi___024root* vlSelf);

VL_ATTR_COLD void Vpqc_accel_axi___024root___eval_settle(Vpqc_accel_axi___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vpqc_accel_axi___024root___eval_settle\n"); );
    Vpqc_accel_axi__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    IData/*31:0*/ __VstlIterCount;
    // Body
    __VstlIterCount = 0U;
    vlSelfRef.__VstlFirstIteration = 1U;
    do {
        if (VL_UNLIKELY(((0x00002710U < __VstlIterCount)))) {
#ifdef VL_DEBUG
            Vpqc_accel_axi___024root___dump_triggers__stl(vlSelfRef.__VstlTriggered, "stl"s);
#endif
            VL_FATAL_MT("/Volumes/Work/projects/code/pqc-hsm-fpga/hardware/rtl/bus/pqc_accel_axi.v", 43, "", "DIDNOTCONVERGE: Settle region did not converge after '--converge-limit' of 10000 tries");
        }
        __VstlIterCount = ((IData)(1U) + __VstlIterCount);
        vlSelfRef.__VstlPhaseResult = Vpqc_accel_axi___024root___eval_phase__stl(vlSelf);
        vlSelfRef.__VstlFirstIteration = 0U;
    } while (vlSelfRef.__VstlPhaseResult);
}

VL_ATTR_COLD bool Vpqc_accel_axi___024root___trigger_anySet__stl(const VlUnpacked<QData/*63:0*/, 1> &in);

#ifdef VL_DEBUG
VL_ATTR_COLD void Vpqc_accel_axi___024root___dump_triggers__stl(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vpqc_accel_axi___024root___dump_triggers__stl\n"); );
    // Body
    if ((1U & (~ (IData)(Vpqc_accel_axi___024root___trigger_anySet__stl(triggers))))) {
        VL_DBG_MSGS("         No '" + tag + "' region triggers active\n");
    }
    if ((1U & (IData)(triggers[0U]))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 0 is active: Internal 'stl' trigger - first iteration\n");
    }
}
#endif  // VL_DEBUG

VL_ATTR_COLD bool Vpqc_accel_axi___024root___trigger_anySet__stl(const VlUnpacked<QData/*63:0*/, 1> &in) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vpqc_accel_axi___024root___trigger_anySet__stl\n"); );
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

extern const VlWide<64>/*2047:0*/ Vpqc_accel_axi__ConstPool__CONST_hd522b744_0;

VL_ATTR_COLD void Vpqc_accel_axi___024root___stl_sequent__TOP__0(Vpqc_accel_axi___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vpqc_accel_axi___024root___stl_sequent__TOP__0\n"); );
    Vpqc_accel_axi__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    CData/*4:0*/ pqc_accel_axi__DOT__u_sha3__DOT__kec_rd_addr;
    pqc_accel_axi__DOT__u_sha3__DOT__kec_rd_addr = 0;
    QData/*63:0*/ pqc_accel_axi__DOT__u_sha3__DOT__kec_rd_data;
    pqc_accel_axi__DOT__u_sha3__DOT__kec_rd_data = 0;
    QData/*63:0*/ pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_3__rc;
    QData/*63:0*/ pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_2__rotl;
    QData/*63:0*/ pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT____VlemCall_0__rotl;
    QData/*63:0*/ __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__0__x;
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__0__x = 0;
    QData/*63:0*/ __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x;
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rotl__2__x = 0;
    CData/*4:0*/ __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rc__3__r;
    __Vfunc_pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__rc__3__r = 0;
    // Body
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pb_we = 0U;
    vlSelfRef.pqc_accel_axi__DOT__mode_ntt = ((7U == vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_mode) 
                                              | (8U 
                                                 == vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__reg_mode));
    vlSelfRef.m_axis_tdata = vlSelfRef.pqc_accel_axi__DOT__bufb_dout;
    vlSelfRef.__VdfgRegularize_hebeb780c_0_1 = (((- (IData)(
                                                            (1U 
                                                             & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_dout) 
                                                                >> 0x0000000fU)))) 
                                                 << 0x00000010U) 
                                                | (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_dout));
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_we = 0U;
    vlSelfRef.s_axi_arready = (1U & (~ ((IData)(vlSelfRef.s_axi_rvalid) 
                                        | (IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__ar_hold))));
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__u_bf_gs_h__DOT__sum 
        = (0x0000ffffU & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_dout) 
                          + (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pb_dout)));
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__u_bf_gs_h__DOT__diff 
        = (0x0000ffffU & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pb_dout) 
                          - (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_dout)));
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__zeroize_rise 
        = ((~ (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__zeroize_d)) 
           & (IData)(vlSelfRef.pqc_accel_axi__DOT__shk_zeroize));
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
    vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_addr 
        = ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__aw_hold)
            ? (IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__aw_addr)
            : (IData)(vlSelfRef.s_axi_awaddr));
    if (vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__w_hold) {
        vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_data 
            = vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__w_data;
        vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_strb 
            = vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__w_strb;
    } else {
        vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_data 
            = vlSelfRef.s_axi_wdata;
        vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_strb 
            = vlSelfRef.s_axi_wstrb;
    }
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_start 
        = ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_start_r) 
           | ((IData)(vlSelfRef.pqc_accel_axi__DOT__kec_start) 
              & (0U == (IData)(vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__state))));
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_addr = 0U;
    vlSelfRef.pqc_accel_axi__DOT__bufa_addr = 0U;
    vlSelfRef.m_axis_tvalid = ((0U != (IData)(vlSelfRef.pqc_accel_axi__DOT__out_words)) 
                               & ((IData)(vlSelfRef.pqc_accel_axi__DOT__rd_ptr) 
                                  < (IData)(vlSelfRef.pqc_accel_axi__DOT__out_words)));
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__j_hi 
        = (0x000001ffU & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__j) 
                          + (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__len)));
    vlSelfRef.pqc_accel_axi__DOT__shk_in_valid = ((7U 
                                                   == (IData)(vlSelfRef.pqc_accel_axi__DOT__state)) 
                                                  & ((IData)(vlSelfRef.pqc_accel_axi__DOT__cnt) 
                                                     != (IData)(vlSelfRef.pqc_accel_axi__DOT__shk_msglen)));
    vlSelfRef.s_axis_tready = (1U & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__busy)));
    vlSelfRef.s_axi_awready = (1U & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__aw_hold)));
    vlSelfRef.s_axi_wready = (1U & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__w_hold)));
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
    vlSelfRef.m_axis_tlast = (((IData)(vlSelfRef.pqc_accel_axi__DOT__rd_ptr) 
                               == (0x000001ffU & ((IData)(vlSelfRef.pqc_accel_axi__DOT__out_words) 
                                                  - (IData)(1U)))) 
                              & (IData)(vlSelfRef.m_axis_tvalid));
    vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pb_addr = 0U;
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_wr_en = 0U;
    vlSelfRef.pqc_accel_axi__DOT__bufa_we = 0U;
    vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__aw_go 
        = ((IData)(vlSelfRef.s_axi_awready) & (IData)(vlSelfRef.s_axi_awvalid));
    vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__w_go 
        = ((IData)(vlSelfRef.s_axi_wready) & (IData)(vlSelfRef.s_axi_wvalid));
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
        if ((1U & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state)))) {
            vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_we = 1U;
            vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pb_addr 
                = (0x000000ffU & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__j_hi));
            vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_din 
                = (0x0000ffffU & ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__inv_r)
                                   ? (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__gs_a_r)
                                   : ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__bf_a_r) 
                                      + (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__u_bf_ct_t__DOT__t))));
        }
        vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_addr 
            = (0x000000ffU & ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state))
                               ? (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__scale_i)
                               : (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__j)));
    } else {
        if ((1U & (~ (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state)))) {
            vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_we 
                = vlSelfRef.pqc_accel_axi__DOT__ntt_wr_en;
            vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pa_din 
                = vlSelfRef.pqc_accel_axi__DOT__ntt_wr_data;
        }
        if ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__state))) {
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
    }
    vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__kec_wr_addr 
        = pqc_accel_axi__DOT__u_sha3__DOT__kec_rd_addr;
    pqc_accel_axi__DOT__u_sha3__DOT__kec_rd_data = 
        (vlSelfRef.pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A
         [pqc_accel_axi__DOT__u_sha3__DOT__kec_rd_addr] 
         & (- (QData)((IData)((0x18U >= (IData)(pqc_accel_axi__DOT__u_sha3__DOT__kec_rd_addr))))));
    vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__wr_fire 
        = (((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__aw_hold) 
            | (IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__aw_go)) 
           & ((~ (IData)(vlSelfRef.s_axi_bvalid)) & 
              ((IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__w_hold) 
               | (IData)(vlSelfRef.pqc_accel_axi__DOT__u_regs__DOT__w_go))));
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
    vlSelfRef.pqc_accel_axi__DOT__bufa_din = 0U;
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
                    vlSelfRef.pqc_accel_axi__DOT__bufa_din 
                        = vlSelfRef.pqc_accel_axi__DOT__sq_next;
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
                vlSelfRef.pqc_accel_axi__DOT__bufa_din 
                    = ((IData)(vlSelfRef.pqc_accel_axi__DOT__is_ntt)
                        ? (((IData)(vlSelfRef.pqc_accel_axi__DOT__u_ntt__DOT__pb_dout) 
                            << 0x00000010U) | (IData)(vlSelfRef.pqc_accel_axi__DOT__lo_lat))
                        : ((1U & (IData)(vlSelfRef.pqc_accel_axi__DOT__cnt))
                            ? (IData)((vlSelfRef.pqc_accel_axi__DOT__kec_rd_data 
                                       >> 0x20U)) : (IData)(vlSelfRef.pqc_accel_axi__DOT__kec_rd_data)));
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
            vlSelfRef.pqc_accel_axi__DOT__bufa_din 
                = vlSelfRef.s_axis_tdata;
        }
    }
}

VL_ATTR_COLD bool Vpqc_accel_axi___024root___eval_phase__stl(Vpqc_accel_axi___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vpqc_accel_axi___024root___eval_phase__stl\n"); );
    Vpqc_accel_axi__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
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
        Vpqc_accel_axi___024root___dump_triggers__stl(vlSelfRef.__VstlTriggered, "stl"s);
    }
#endif
    __VstlExecute = Vpqc_accel_axi___024root___trigger_anySet__stl(vlSelfRef.__VstlTriggered);
    if (__VstlExecute) {
        {
            // Inlined CFunc: _eval_stl
            if ((1ULL & vlSelfRef.__VstlTriggered[0U])) {
                Vpqc_accel_axi___024root___stl_sequent__TOP__0(vlSelf);
            }
        }
    }
    return (__VstlExecute);
}

bool Vpqc_accel_axi___024root___trigger_anySet__ico(const VlUnpacked<QData/*63:0*/, 2> &in);

#ifdef VL_DEBUG
VL_ATTR_COLD void Vpqc_accel_axi___024root___dump_triggers__ico(const VlUnpacked<QData/*63:0*/, 2> &triggers, const std::string &tag) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vpqc_accel_axi___024root___dump_triggers__ico\n"); );
    // Body
    if ((1U & (~ (IData)(Vpqc_accel_axi___024root___trigger_anySet__ico(triggers))))) {
        VL_DBG_MSGS("         No '" + tag + "' region triggers active\n");
    }
    if ((1U & (IData)(triggers[0U]))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 0 is active: @( clk)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 1U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 1 is active: @( rst_n)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 2U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 2 is active: @( s_axi_awaddr)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 3U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 3 is active: @( s_axi_awvalid)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 4U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 4 is active: @( s_axi_wdata)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 5U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 5 is active: @( s_axi_wstrb)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 6U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 6 is active: @( s_axi_wvalid)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 7U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 7 is active: @( s_axi_bready)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 8U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 8 is active: @( s_axi_araddr)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 9U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 9 is active: @( s_axi_arvalid)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 0x0000000aU)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 10 is active: @( s_axi_rready)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 0x0000000bU)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 11 is active: @( s_axis_tdata)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 0x0000000cU)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 12 is active: @( s_axis_tvalid)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 0x0000000dU)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 13 is active: @( s_axis_tlast)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 0x0000000eU)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 14 is active: @( m_axis_tready)\n");
    }
    if ((1U & (IData)(triggers[1U]))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 64 is active: Internal 'ico' trigger - first iteration\n");
    }
}
#endif  // VL_DEBUG

bool Vpqc_accel_axi___024root___trigger_anySet__act(const VlUnpacked<QData/*63:0*/, 1> &in);

#ifdef VL_DEBUG
VL_ATTR_COLD void Vpqc_accel_axi___024root___dump_triggers__act(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vpqc_accel_axi___024root___dump_triggers__act\n"); );
    // Body
    if ((1U & (~ (IData)(Vpqc_accel_axi___024root___trigger_anySet__act(triggers))))) {
        VL_DBG_MSGS("         No '" + tag + "' region triggers active\n");
    }
    if ((1U & (IData)(triggers[0U]))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 0 is active: @(posedge clk)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 1U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 1 is active: @(negedge rst_n)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 2U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 2 is active: @(negedge pqc_accel_axi.core_rst_n)\n");
    }
}
#endif  // VL_DEBUG

VL_ATTR_COLD void Vpqc_accel_axi___024root___ctor_var_reset(Vpqc_accel_axi___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vpqc_accel_axi___024root___ctor_var_reset\n"); );
    Vpqc_accel_axi__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    const uint64_t __VscopeHash = VL_MURMUR64_HASH(vlSelf->vlNamep);
    vlSelf->clk = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 16707436170211756652ull);
    vlSelf->rst_n = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 1638864771569018232ull);
    vlSelf->s_axi_awaddr = VL_SCOPED_RAND_RESET_I(8, __VscopeHash, 7303631981020876172ull);
    vlSelf->s_axi_awvalid = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 13986037914296269070ull);
    vlSelf->s_axi_awready = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 14099717354022636468ull);
    vlSelf->s_axi_wdata = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 11311253403970331505ull);
    vlSelf->s_axi_wstrb = VL_SCOPED_RAND_RESET_I(4, __VscopeHash, 18112015138521062007ull);
    vlSelf->s_axi_wvalid = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 12168530306759773544ull);
    vlSelf->s_axi_wready = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 17608475915581965368ull);
    vlSelf->s_axi_bresp = VL_SCOPED_RAND_RESET_I(2, __VscopeHash, 15162762900795686431ull);
    vlSelf->s_axi_bvalid = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 9334582144896637853ull);
    vlSelf->s_axi_bready = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 15653039750784194130ull);
    vlSelf->s_axi_araddr = VL_SCOPED_RAND_RESET_I(8, __VscopeHash, 8722301305194254610ull);
    vlSelf->s_axi_arvalid = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 17746383479076595557ull);
    vlSelf->s_axi_arready = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 17791137924766170856ull);
    vlSelf->s_axi_rdata = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 12866136205313389248ull);
    vlSelf->s_axi_rresp = VL_SCOPED_RAND_RESET_I(2, __VscopeHash, 14929039895447920609ull);
    vlSelf->s_axi_rvalid = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 15026938065200214434ull);
    vlSelf->s_axi_rready = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 1794163653381394343ull);
    vlSelf->s_axis_tdata = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 6413635470731068441ull);
    vlSelf->s_axis_tvalid = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 7678217216116487763ull);
    vlSelf->s_axis_tready = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 12236809265553805965ull);
    vlSelf->s_axis_tlast = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 750346815483064505ull);
    vlSelf->m_axis_tdata = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 8796747702141925029ull);
    vlSelf->m_axis_tvalid = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 11050073027672567459ull);
    vlSelf->m_axis_tready = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 5619517951140101778ull);
    vlSelf->m_axis_tlast = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 3666489172703407315ull);
    vlSelf->pqc_accel_axi__DOT__bufa_addr = VL_SCOPED_RAND_RESET_I(7, __VscopeHash, 8442061412210865991ull);
    vlSelf->pqc_accel_axi__DOT__bufa_we = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 17535693053885434490ull);
    vlSelf->pqc_accel_axi__DOT__bufa_din = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 15126162417043922419ull);
    vlSelf->pqc_accel_axi__DOT__bufa_dout = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 4102019067973862745ull);
    vlSelf->pqc_accel_axi__DOT__bufb_dout = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 16893698194454651473ull);
    vlSelf->pqc_accel_axi__DOT__wr_ptr = VL_SCOPED_RAND_RESET_I(9, __VscopeHash, 16329680240237482767ull);
    vlSelf->pqc_accel_axi__DOT__rd_ptr = VL_SCOPED_RAND_RESET_I(9, __VscopeHash, 16088961210466723188ull);
    vlSelf->pqc_accel_axi__DOT__out_words = VL_SCOPED_RAND_RESET_I(9, __VscopeHash, 8983320770316945787ull);
    vlSelf->pqc_accel_axi__DOT__done_set = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 5523274750529841847ull);
    vlSelf->pqc_accel_axi__DOT__err_set = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 14496935803772217024ull);
    vlSelf->pqc_accel_axi__DOT__out_we = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 15215595756781701999ull);
    vlSelf->pqc_accel_axi__DOT__out_len_r = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 11068570491951126652ull);
    vlSelf->pqc_accel_axi__DOT__errcode_r = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 10614628493237893214ull);
    vlSelf->pqc_accel_axi__DOT__busy = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 14013203661909118892ull);
    vlSelf->pqc_accel_axi__DOT__core_rst_n = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 16423373347859007530ull);
    vlSelf->pqc_accel_axi__DOT__state = VL_SCOPED_RAND_RESET_I(4, __VscopeHash, 13296562043418308021ull);
    vlSelf->pqc_accel_axi__DOT__cnt = VL_SCOPED_RAND_RESET_I(10, __VscopeHash, 8636476000271535816ull);
    vlSelf->pqc_accel_axi__DOT__is_ntt = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 9347265450816885987ull);
    vlSelf->pqc_accel_axi__DOT__lo_lat = VL_SCOPED_RAND_RESET_I(16, __VscopeHash, 11499399135118097550ull);
    vlSelf->pqc_accel_axi__DOT__lane_lo = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 8461970811356707122ull);
    vlSelf->pqc_accel_axi__DOT__ntt_start = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 12895698501854336285ull);
    vlSelf->pqc_accel_axi__DOT__ntt_inverse = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 10691198489704600403ull);
    vlSelf->pqc_accel_axi__DOT__ntt_wr_en = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 11047547684559492474ull);
    vlSelf->pqc_accel_axi__DOT__ntt_wr_addr = VL_SCOPED_RAND_RESET_I(8, __VscopeHash, 11299597415080794367ull);
    vlSelf->pqc_accel_axi__DOT__ntt_wr_data = VL_SCOPED_RAND_RESET_I(16, __VscopeHash, 4852926202147223456ull);
    vlSelf->pqc_accel_axi__DOT__ntt_rd_addr = VL_SCOPED_RAND_RESET_I(8, __VscopeHash, 9648922385783780803ull);
    vlSelf->pqc_accel_axi__DOT__ntt_done = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 16091120334128301040ull);
    vlSelf->pqc_accel_axi__DOT__kec_start = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 15606487280407235147ull);
    vlSelf->pqc_accel_axi__DOT__kec_wr_en = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 11899760217198759050ull);
    vlSelf->pqc_accel_axi__DOT__kec_wr_addr = VL_SCOPED_RAND_RESET_I(5, __VscopeHash, 796414053958329442ull);
    vlSelf->pqc_accel_axi__DOT__kec_wr_data = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 6841202220495041567ull);
    vlSelf->pqc_accel_axi__DOT__kec_rd_addr = VL_SCOPED_RAND_RESET_I(5, __VscopeHash, 14276175942653257457ull);
    vlSelf->pqc_accel_axi__DOT__kec_rd_data = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 5861690551422945599ull);
    vlSelf->pqc_accel_axi__DOT__shk_start = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 7439152444750022429ull);
    vlSelf->pqc_accel_axi__DOT__shk_flush = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 1797662427925643746ull);
    vlSelf->pqc_accel_axi__DOT__shk_zeroize = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 6961795916587733198ull);
    vlSelf->pqc_accel_axi__DOT__shk_rate = VL_SCOPED_RAND_RESET_I(8, __VscopeHash, 824617538021111281ull);
    vlSelf->pqc_accel_axi__DOT__shk_suffix = VL_SCOPED_RAND_RESET_I(8, __VscopeHash, 3785218240739699208ull);
    vlSelf->pqc_accel_axi__DOT__shk_msglen = VL_SCOPED_RAND_RESET_I(10, __VscopeHash, 8290085188352233565ull);
    vlSelf->pqc_accel_axi__DOT__shk_outlen = VL_SCOPED_RAND_RESET_I(10, __VscopeHash, 17174937599353599914ull);
    vlSelf->pqc_accel_axi__DOT__shk_in_valid = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 9905177639264806113ull);
    vlSelf->pqc_accel_axi__DOT__mode_ntt = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 3760965109939942854ull);
    vlSelf->pqc_accel_axi__DOT__sq_acc = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 12894329362614626904ull);
    vlSelf->pqc_accel_axi__DOT__sq_next = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 16863777127143925432ull);
    for (int __Vi0 = 0; __Vi0 < 128; ++__Vi0) {
        vlSelf->pqc_accel_axi__DOT__u_ntt__DOT__zetas[__Vi0] = VL_SCOPED_RAND_RESET_I(16, __VscopeHash, 1290012614242275240ull);
    }
    vlSelf->pqc_accel_axi__DOT__u_ntt__DOT__state = VL_SCOPED_RAND_RESET_I(3, __VscopeHash, 9035656977912416715ull);
    vlSelf->pqc_accel_axi__DOT__u_ntt__DOT__len = VL_SCOPED_RAND_RESET_I(9, __VscopeHash, 2556566181629281590ull);
    vlSelf->pqc_accel_axi__DOT__u_ntt__DOT__grp = VL_SCOPED_RAND_RESET_I(9, __VscopeHash, 8906996921150810820ull);
    vlSelf->pqc_accel_axi__DOT__u_ntt__DOT__j = VL_SCOPED_RAND_RESET_I(9, __VscopeHash, 7827831673537440020ull);
    vlSelf->pqc_accel_axi__DOT__u_ntt__DOT__k = VL_SCOPED_RAND_RESET_I(8, __VscopeHash, 3817603622773141361ull);
    vlSelf->pqc_accel_axi__DOT__u_ntt__DOT__inv_r = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 2998239247289273083ull);
    vlSelf->pqc_accel_axi__DOT__u_ntt__DOT__scale_i = VL_SCOPED_RAND_RESET_I(9, __VscopeHash, 11227914033045800300ull);
    vlSelf->pqc_accel_axi__DOT__u_ntt__DOT__j_hi = VL_SCOPED_RAND_RESET_I(9, __VscopeHash, 15922319694148158249ull);
    vlSelf->pqc_accel_axi__DOT__u_ntt__DOT__pa_we = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 4814210108099418740ull);
    vlSelf->pqc_accel_axi__DOT__u_ntt__DOT__pb_we = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 12321159539348964453ull);
    vlSelf->pqc_accel_axi__DOT__u_ntt__DOT__pa_addr = VL_SCOPED_RAND_RESET_I(8, __VscopeHash, 11661508417915777457ull);
    vlSelf->pqc_accel_axi__DOT__u_ntt__DOT__pb_addr = VL_SCOPED_RAND_RESET_I(8, __VscopeHash, 1673623465657397078ull);
    vlSelf->pqc_accel_axi__DOT__u_ntt__DOT__pa_din = VL_SCOPED_RAND_RESET_I(16, __VscopeHash, 4654506940118419265ull);
    vlSelf->pqc_accel_axi__DOT__u_ntt__DOT__pb_din = VL_SCOPED_RAND_RESET_I(16, __VscopeHash, 14415539170351020790ull);
    vlSelf->pqc_accel_axi__DOT__u_ntt__DOT__pa_dout = VL_SCOPED_RAND_RESET_I(16, __VscopeHash, 7951044865640697587ull);
    vlSelf->pqc_accel_axi__DOT__u_ntt__DOT__pb_dout = VL_SCOPED_RAND_RESET_I(16, __VscopeHash, 4071994232370107987ull);
    vlSelf->pqc_accel_axi__DOT__u_ntt__DOT__prod_r = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 17103814726452960148ull);
    vlSelf->pqc_accel_axi__DOT__u_ntt__DOT__bf_a_r = VL_SCOPED_RAND_RESET_I(16, __VscopeHash, 10600653416658644747ull);
    vlSelf->pqc_accel_axi__DOT__u_ntt__DOT__gs_a_r = VL_SCOPED_RAND_RESET_I(16, __VscopeHash, 5223271449650515190ull);
    vlSelf->pqc_accel_axi__DOT__u_ntt__DOT__scprod_r = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 66759321372678455ull);
    vlSelf->pqc_accel_axi__DOT__u_ntt__DOT__scbarr_r = VL_SCOPED_RAND_RESET_I(16, __VscopeHash, 18119962374318717360ull);
    vlSelf->pqc_accel_axi__DOT__u_ntt__DOT__u_bf_ct_t__DOT__t = VL_SCOPED_RAND_RESET_I(16, __VscopeHash, 2699674838162882884ull);
    vlSelf->pqc_accel_axi__DOT__u_ntt__DOT__u_bf_gs_h__DOT__sum = VL_SCOPED_RAND_RESET_I(16, __VscopeHash, 10121935840255813668ull);
    vlSelf->pqc_accel_axi__DOT__u_ntt__DOT__u_bf_gs_h__DOT__diff = VL_SCOPED_RAND_RESET_I(16, __VscopeHash, 12911518198504611155ull);
    for (int __Vi0 = 0; __Vi0 < 256; ++__Vi0) {
        vlSelf->pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem[__Vi0] = VL_SCOPED_RAND_RESET_I(16, __VscopeHash, 4824535734362033961ull);
    }
    vlSelf->pqc_accel_axi__DOT__u_sha3__DOT__state = VL_SCOPED_RAND_RESET_I(4, __VscopeHash, 1038094638234149200ull);
    vlSelf->pqc_accel_axi__DOT__u_sha3__DOT__ret = VL_SCOPED_RAND_RESET_I(2, __VscopeHash, 8227901403787053472ull);
    vlSelf->pqc_accel_axi__DOT__u_sha3__DOT__rate_r = VL_SCOPED_RAND_RESET_I(8, __VscopeHash, 11548657177581933880ull);
    vlSelf->pqc_accel_axi__DOT__u_sha3__DOT__suffix_r = VL_SCOPED_RAND_RESET_I(8, __VscopeHash, 3264137971798738994ull);
    vlSelf->pqc_accel_axi__DOT__u_sha3__DOT__bpos = VL_SCOPED_RAND_RESET_I(8, __VscopeHash, 4219296994816895109ull);
    vlSelf->pqc_accel_axi__DOT__u_sha3__DOT__sq_bpos = VL_SCOPED_RAND_RESET_I(8, __VscopeHash, 13700741090544584449ull);
    vlSelf->pqc_accel_axi__DOT__u_sha3__DOT__clr_idx = VL_SCOPED_RAND_RESET_I(5, __VscopeHash, 6833551664947851126ull);
    vlSelf->pqc_accel_axi__DOT__u_sha3__DOT__kec_start_r = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 17412876385731906805ull);
    vlSelf->pqc_accel_axi__DOT__u_sha3__DOT__kec_start = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 12250387855582125690ull);
    vlSelf->pqc_accel_axi__DOT__u_sha3__DOT__kec_done = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 275542213901515273ull);
    vlSelf->pqc_accel_axi__DOT__u_sha3__DOT__kec_wr_en = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 10620405056740002150ull);
    vlSelf->pqc_accel_axi__DOT__u_sha3__DOT__kec_wr_addr = VL_SCOPED_RAND_RESET_I(5, __VscopeHash, 13802672085528652304ull);
    vlSelf->pqc_accel_axi__DOT__u_sha3__DOT__kec_wr_data = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 5033183576488855272ull);
    vlSelf->pqc_accel_axi__DOT__u_sha3__DOT__perm_busy = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 17510115807935815526ull);
    vlSelf->pqc_accel_axi__DOT__u_sha3__DOT__zeroize_d = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 16047439616379135334ull);
    vlSelf->pqc_accel_axi__DOT__u_sha3__DOT__zeroize_rise = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 1707792304128635041ull);
    for (int __Vi0 = 0; __Vi0 < 25; ++__Vi0) {
        vlSelf->pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A[__Vi0] = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 5518440229279630213ull);
    }
    vlSelf->pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__round_cnt = VL_SCOPED_RAND_RESET_I(5, __VscopeHash, 3185914315778501167ull);
    vlSelf->pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__busy = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 3624417023317144288ull);
    for (int __Vi0 = 0; __Vi0 < 5; ++__Vi0) {
        vlSelf->pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__C[__Vi0] = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 9193187507047375459ull);
    }
    for (int __Vi0 = 0; __Vi0 < 5; ++__Vi0) {
        vlSelf->pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__D[__Vi0] = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 14971570878775338479ull);
    }
    for (int __Vi0 = 0; __Vi0 < 25; ++__Vi0) {
        vlSelf->pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Ath[__Vi0] = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 8015802902422468575ull);
    }
    for (int __Vi0 = 0; __Vi0 < 25; ++__Vi0) {
        vlSelf->pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__B[__Vi0] = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 9369090529687036345ull);
    }
    for (int __Vi0 = 0; __Vi0 < 25; ++__Vi0) {
        vlSelf->pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__Anext[__Vi0] = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 10242173854924849694ull);
    }
    for (int __Vi0 = 0; __Vi0 < 128; ++__Vi0) {
        vlSelf->pqc_accel_axi__DOT__u_buf__DOT__mem[__Vi0] = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 8495709752112867643ull);
    }
    vlSelf->pqc_accel_axi__DOT__u_regs__DOT__reg_mode = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 13325012440842118319ull);
    vlSelf->pqc_accel_axi__DOT__u_regs__DOT__reg_param = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 9643079574657223802ull);
    vlSelf->pqc_accel_axi__DOT__u_regs__DOT__reg_in_len = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 1290527406786574020ull);
    vlSelf->pqc_accel_axi__DOT__u_regs__DOT__reg_out_len = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 3317678949155600707ull);
    vlSelf->pqc_accel_axi__DOT__u_regs__DOT__reg_errcode = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 8826443075949329376ull);
    vlSelf->pqc_accel_axi__DOT__u_regs__DOT__st_done = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 7728916874813221571ull);
    vlSelf->pqc_accel_axi__DOT__u_regs__DOT__st_err = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 10722394102231873335ull);
    vlSelf->pqc_accel_axi__DOT__u_regs__DOT__start_r = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 16451663255679770873ull);
    vlSelf->pqc_accel_axi__DOT__u_regs__DOT__soft_reset_r = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 7405309183732446593ull);
    vlSelf->pqc_accel_axi__DOT__u_regs__DOT__aw_hold = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 5251345471153647038ull);
    vlSelf->pqc_accel_axi__DOT__u_regs__DOT__w_hold = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 773096142258088635ull);
    vlSelf->pqc_accel_axi__DOT__u_regs__DOT__aw_addr = VL_SCOPED_RAND_RESET_I(8, __VscopeHash, 18160879350121466569ull);
    vlSelf->pqc_accel_axi__DOT__u_regs__DOT__w_data = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 11249581164273791884ull);
    vlSelf->pqc_accel_axi__DOT__u_regs__DOT__w_strb = VL_SCOPED_RAND_RESET_I(4, __VscopeHash, 18386768107912583836ull);
    vlSelf->pqc_accel_axi__DOT__u_regs__DOT__aw_go = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 4683167335834553445ull);
    vlSelf->pqc_accel_axi__DOT__u_regs__DOT__w_go = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 14950980387024839256ull);
    vlSelf->pqc_accel_axi__DOT__u_regs__DOT__wr_fire = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 17949875536498563984ull);
    vlSelf->pqc_accel_axi__DOT__u_regs__DOT__wr_addr = VL_SCOPED_RAND_RESET_I(8, __VscopeHash, 1948703167538311861ull);
    vlSelf->pqc_accel_axi__DOT__u_regs__DOT__wr_data = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 13316517799575881733ull);
    vlSelf->pqc_accel_axi__DOT__u_regs__DOT__wr_strb = VL_SCOPED_RAND_RESET_I(4, __VscopeHash, 10292505512217498773ull);
    vlSelf->pqc_accel_axi__DOT__u_regs__DOT__ar_addr = VL_SCOPED_RAND_RESET_I(8, __VscopeHash, 6287846874346931226ull);
    vlSelf->pqc_accel_axi__DOT__u_regs__DOT__ar_hold = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 17295165528176162444ull);
    vlSelf->__VdfgRegularize_hebeb780c_0_1 = 0;
    vlSelf->__VdfgRegularize_hebeb780c_0_2 = 0;
    vlSelf->__Vdly__pqc_accel_axi__DOT__state = 0;
    vlSelf->__Vdly__pqc_accel_axi__DOT__cnt = 0;
    vlSelf->__Vdly__pqc_accel_axi__DOT__rd_ptr = 0;
    vlSelf->__Vdly__pqc_accel_axi__DOT__is_ntt = 0;
    vlSelf->__Vdly__pqc_accel_axi__DOT__ntt_start = 0;
    vlSelf->__Vdly__pqc_accel_axi__DOT__ntt_inverse = 0;
    vlSelf->__Vdly__pqc_accel_axi__DOT__kec_start = 0;
    vlSelf->__Vdly__pqc_accel_axi__DOT__lane_lo = 0;
    vlSelf->__Vdly__pqc_accel_axi__DOT__shk_msglen = 0;
    vlSelf->__Vdly__pqc_accel_axi__DOT__shk_outlen = 0;
    vlSelf->__Vdly__pqc_accel_axi__DOT__u_regs__DOT__reg_in_len = 0;
    vlSelf->__Vdly__pqc_accel_axi__DOT__u_regs__DOT__reg_param = 0;
    vlSelf->__Vdly__pqc_accel_axi__DOT__u_regs__DOT__reg_mode = 0;
    vlSelf->__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__state = 0;
    vlSelf->__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__grp = 0;
    vlSelf->__Vdly__pqc_accel_axi__DOT__u_ntt__DOT__len = 0;
    vlSelf->__Vdly__pqc_accel_axi__DOT__u_sha3__DOT__state = 0;
    vlSelf->__VdlySet__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v0 = 0;
    vlSelf->__VdlySet__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v1 = 0;
    vlSelf->__VdlySet__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v25 = 0;
    vlSelf->__VdlySet__pqc_accel_axi__DOT__u_sha3__DOT__u_kec__DOT__A__v26 = 0;
    vlSelf->__VdlyVal__pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem__v0 = 0;
    vlSelf->__VdlyDim0__pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem__v0 = 0;
    vlSelf->__VdlySet__pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem__v0 = 0;
    vlSelf->__VdlyVal__pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem__v1 = 0;
    vlSelf->__VdlyDim0__pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem__v1 = 0;
    vlSelf->__VdlySet__pqc_accel_axi__DOT__u_ntt__DOT__u_mem__DOT__mem__v1 = 0;
    vlSelf->__VdlyVal__pqc_accel_axi__DOT__u_buf__DOT__mem__v0 = 0;
    vlSelf->__VdlyDim0__pqc_accel_axi__DOT__u_buf__DOT__mem__v0 = 0;
    vlSelf->__VdlySet__pqc_accel_axi__DOT__u_buf__DOT__mem__v0 = 0;
    for (int __Vi0 = 0; __Vi0 < 1; ++__Vi0) {
        vlSelf->__VstlTriggered[__Vi0] = 0;
    }
    for (int __Vi0 = 0; __Vi0 < 2; ++__Vi0) {
        vlSelf->__VicoTriggered[__Vi0] = 0;
    }
    vlSelf->__Vtrigprevexpr___TOP__clk__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__rst_n__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__s_axi_awaddr__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__s_axi_awvalid__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__s_axi_wdata__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__s_axi_wstrb__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__s_axi_wvalid__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__s_axi_bready__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__s_axi_araddr__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__s_axi_arvalid__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__s_axi_rready__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__s_axis_tdata__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__s_axis_tvalid__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__s_axis_tlast__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__m_axis_tready__0 = 0;
    vlSelf->__VicoDidInit = 0;
    for (int __Vi0 = 0; __Vi0 < 1; ++__Vi0) {
        vlSelf->__VactTriggered[__Vi0] = 0;
    }
    vlSelf->__Vtrigprevexpr___TOP__clk__1 = 0;
    vlSelf->__Vtrigprevexpr___TOP__rst_n__1 = 0;
    vlSelf->__Vtrigprevexpr___TOP__pqc_accel_axi__DOT__core_rst_n__0 = 0;
    for (int __Vi0 = 0; __Vi0 < 1; ++__Vi0) {
        vlSelf->__VnbaTriggered[__Vi0] = 0;
    }
}
