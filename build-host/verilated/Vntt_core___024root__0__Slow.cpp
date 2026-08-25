// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design implementation internals
// See Vntt_core.h for the primary calling header

#include "Vntt_core__pch.h"

VL_ATTR_COLD void Vntt_core___024root___eval_static(Vntt_core___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vntt_core___024root___eval_static\n"); );
    Vntt_core__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    vlSelfRef.__Vtrigprevexpr___TOP__clk__0 = vlSelfRef.clk;
    vlSelfRef.__Vtrigprevexpr___TOP__rst_n__0 = vlSelfRef.rst_n;
    vlSelfRef.__Vtrigprevexpr___TOP__start__0 = vlSelfRef.start;
    vlSelfRef.__Vtrigprevexpr___TOP__inverse__0 = vlSelfRef.inverse;
    vlSelfRef.__Vtrigprevexpr___TOP__wr_en__0 = vlSelfRef.wr_en;
    vlSelfRef.__Vtrigprevexpr___TOP__wr_addr__0 = vlSelfRef.wr_addr;
    vlSelfRef.__Vtrigprevexpr___TOP__wr_data__0 = vlSelfRef.wr_data;
    vlSelfRef.__Vtrigprevexpr___TOP__rd_addr__0 = vlSelfRef.rd_addr;
    vlSelfRef.__Vtrigprevexpr___TOP__clk__1 = vlSelfRef.clk;
    vlSelfRef.__Vtrigprevexpr___TOP__rst_n__1 = vlSelfRef.rst_n;
}

VL_ATTR_COLD void Vntt_core___024root___eval_initial__TOP(Vntt_core___024root* vlSelf);

VL_ATTR_COLD void Vntt_core___024root___eval_initial(Vntt_core___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vntt_core___024root___eval_initial\n"); );
    Vntt_core__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    Vntt_core___024root___eval_initial__TOP(vlSelf);
}

VL_ATTR_COLD void Vntt_core___024root___eval_initial__TOP(Vntt_core___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vntt_core___024root___eval_initial__TOP\n"); );
    Vntt_core__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    IData/*31:0*/ ntt_core__DOT__u_mem__DOT__i;
    ntt_core__DOT__u_mem__DOT__i = 0;
    // Body
    vlSelfRef.ntt_core__DOT__zetas[0U] = 0xfbecU;
    vlSelfRef.ntt_core__DOT__zetas[1U] = 0xfd0aU;
    vlSelfRef.ntt_core__DOT__zetas[2U] = 0xfe99U;
    vlSelfRef.ntt_core__DOT__zetas[3U] = 0xfa13U;
    vlSelfRef.ntt_core__DOT__zetas[4U] = 0x05d5U;
    vlSelfRef.ntt_core__DOT__zetas[5U] = 0x058eU;
    vlSelfRef.ntt_core__DOT__zetas[6U] = 0x011fU;
    vlSelfRef.ntt_core__DOT__zetas[7U] = 0x00caU;
    vlSelfRef.ntt_core__DOT__zetas[8U] = 0xff55U;
    vlSelfRef.ntt_core__DOT__zetas[9U] = 0x026eU;
    vlSelfRef.ntt_core__DOT__zetas[10U] = 0x0629U;
    vlSelfRef.ntt_core__DOT__zetas[11U] = 0x00b6U;
    vlSelfRef.ntt_core__DOT__zetas[12U] = 0x03c2U;
    vlSelfRef.ntt_core__DOT__zetas[13U] = 0xfb4eU;
    vlSelfRef.ntt_core__DOT__zetas[14U] = 0xfa3eU;
    vlSelfRef.ntt_core__DOT__zetas[15U] = 0x05bcU;
    vlSelfRef.ntt_core__DOT__zetas[16U] = 0x023dU;
    vlSelfRef.ntt_core__DOT__zetas[17U] = 0xfad3U;
    vlSelfRef.ntt_core__DOT__zetas[18U] = 0x0108U;
    vlSelfRef.ntt_core__DOT__zetas[19U] = 0x017fU;
    vlSelfRef.ntt_core__DOT__zetas[20U] = 0xfcc3U;
    vlSelfRef.ntt_core__DOT__zetas[21U] = 0x05b2U;
    vlSelfRef.ntt_core__DOT__zetas[22U] = 0xf9beU;
    vlSelfRef.ntt_core__DOT__zetas[23U] = 0xff7eU;
    vlSelfRef.ntt_core__DOT__zetas[24U] = 0xfd57U;
    vlSelfRef.ntt_core__DOT__zetas[25U] = 0x03f9U;
    vlSelfRef.ntt_core__DOT__zetas[26U] = 0x02dcU;
    vlSelfRef.ntt_core__DOT__zetas[27U] = 0x0260U;
    vlSelfRef.ntt_core__DOT__zetas[28U] = 0xf9faU;
    vlSelfRef.ntt_core__DOT__zetas[29U] = 0x019bU;
    vlSelfRef.ntt_core__DOT__zetas[30U] = 0xff33U;
    vlSelfRef.ntt_core__DOT__zetas[31U] = 0xf9ddU;
    vlSelfRef.ntt_core__DOT__zetas[32U] = 0x04c7U;
    vlSelfRef.ntt_core__DOT__zetas[33U] = 0x028cU;
    vlSelfRef.ntt_core__DOT__zetas[34U] = 0xfdd8U;
    vlSelfRef.ntt_core__DOT__zetas[35U] = 0x03f7U;
    vlSelfRef.ntt_core__DOT__zetas[36U] = 0xfaf3U;
    vlSelfRef.ntt_core__DOT__zetas[37U] = 0x05d3U;
    vlSelfRef.ntt_core__DOT__zetas[38U] = 0xfee6U;
    vlSelfRef.ntt_core__DOT__zetas[39U] = 0xf9f8U;
    vlSelfRef.ntt_core__DOT__zetas[40U] = 0x0204U;
    vlSelfRef.ntt_core__DOT__zetas[41U] = 0xfff8U;
    vlSelfRef.ntt_core__DOT__zetas[42U] = 0xfec0U;
    vlSelfRef.ntt_core__DOT__zetas[43U] = 0xfd66U;
    vlSelfRef.ntt_core__DOT__zetas[44U] = 0xf9aeU;
    vlSelfRef.ntt_core__DOT__zetas[45U] = 0xfb76U;
    vlSelfRef.ntt_core__DOT__zetas[46U] = 0x007eU;
    vlSelfRef.ntt_core__DOT__zetas[47U] = 0x05bdU;
    vlSelfRef.ntt_core__DOT__zetas[48U] = 0xfcabU;
    vlSelfRef.ntt_core__DOT__zetas[49U] = 0xffa6U;
    vlSelfRef.ntt_core__DOT__zetas[50U] = 0xfef1U;
    vlSelfRef.ntt_core__DOT__zetas[51U] = 0x033eU;
    vlSelfRef.ntt_core__DOT__zetas[52U] = 0x006bU;
    vlSelfRef.ntt_core__DOT__zetas[53U] = 0xfa73U;
    vlSelfRef.ntt_core__DOT__zetas[54U] = 0xff09U;
    vlSelfRef.ntt_core__DOT__zetas[55U] = 0xfc49U;
    vlSelfRef.ntt_core__DOT__zetas[56U] = 0xfe72U;
    vlSelfRef.ntt_core__DOT__zetas[57U] = 0x03c1U;
    vlSelfRef.ntt_core__DOT__zetas[58U] = 0xfa1cU;
    vlSelfRef.ntt_core__DOT__zetas[59U] = 0xfd2bU;
    vlSelfRef.ntt_core__DOT__zetas[60U] = 0x01c0U;
    vlSelfRef.ntt_core__DOT__zetas[61U] = 0xfbd7U;
    vlSelfRef.ntt_core__DOT__zetas[62U] = 0x02a5U;
    vlSelfRef.ntt_core__DOT__zetas[63U] = 0xfb05U;
    vlSelfRef.ntt_core__DOT__zetas[64U] = 0xfbb1U;
    vlSelfRef.ntt_core__DOT__zetas[65U] = 0x01aeU;
    vlSelfRef.ntt_core__DOT__zetas[66U] = 0x022bU;
    vlSelfRef.ntt_core__DOT__zetas[67U] = 0x034bU;
    vlSelfRef.ntt_core__DOT__zetas[68U] = 0xfb1dU;
    vlSelfRef.ntt_core__DOT__zetas[69U] = 0x0367U;
    vlSelfRef.ntt_core__DOT__zetas[70U] = 0x060eU;
    vlSelfRef.ntt_core__DOT__zetas[71U] = 0x0069U;
    vlSelfRef.ntt_core__DOT__zetas[72U] = 0x01a6U;
    vlSelfRef.ntt_core__DOT__zetas[73U] = 0x024bU;
    vlSelfRef.ntt_core__DOT__zetas[74U] = 0x00b1U;
    vlSelfRef.ntt_core__DOT__zetas[75U] = 0xff15U;
    vlSelfRef.ntt_core__DOT__zetas[76U] = 0xfeddU;
    vlSelfRef.ntt_core__DOT__zetas[77U] = 0xfe34U;
    vlSelfRef.ntt_core__DOT__zetas[78U] = 0x0626U;
    vlSelfRef.ntt_core__DOT__zetas[79U] = 0x0675U;
    vlSelfRef.ntt_core__DOT__zetas[80U] = 0xff0aU;
    vlSelfRef.ntt_core__DOT__zetas[81U] = 0x030aU;
    vlSelfRef.ntt_core__DOT__zetas[82U] = 0x0487U;
    vlSelfRef.ntt_core__DOT__zetas[83U] = 0xff6dU;
    vlSelfRef.ntt_core__DOT__zetas[84U] = 0xfcf7U;
    vlSelfRef.ntt_core__DOT__zetas[85U] = 0x05cbU;
    vlSelfRef.ntt_core__DOT__zetas[86U] = 0xfda6U;
    vlSelfRef.ntt_core__DOT__zetas[87U] = 0x045fU;
    vlSelfRef.ntt_core__DOT__zetas[88U] = 0xf9caU;
    vlSelfRef.ntt_core__DOT__zetas[89U] = 0x0284U;
    vlSelfRef.ntt_core__DOT__zetas[90U] = 0xfc98U;
    vlSelfRef.ntt_core__DOT__zetas[91U] = 0x015dU;
    vlSelfRef.ntt_core__DOT__zetas[92U] = 0x01a2U;
    vlSelfRef.ntt_core__DOT__zetas[93U] = 0x0149U;
    vlSelfRef.ntt_core__DOT__zetas[94U] = 0xff64U;
    vlSelfRef.ntt_core__DOT__zetas[95U] = 0xffb5U;
    vlSelfRef.ntt_core__DOT__zetas[96U] = 0x0331U;
    vlSelfRef.ntt_core__DOT__zetas[97U] = 0x0449U;
    vlSelfRef.ntt_core__DOT__zetas[98U] = 0x025bU;
    vlSelfRef.ntt_core__DOT__zetas[99U] = 0x0262U;
    vlSelfRef.ntt_core__DOT__zetas[100U] = 0x052aU;
    vlSelfRef.ntt_core__DOT__zetas[101U] = 0xfafbU;
    vlSelfRef.ntt_core__DOT__zetas[102U] = 0xfa47U;
    vlSelfRef.ntt_core__DOT__zetas[103U] = 0x0180U;
    vlSelfRef.ntt_core__DOT__zetas[104U] = 0xfb41U;
    vlSelfRef.ntt_core__DOT__zetas[105U] = 0xff78U;
    vlSelfRef.ntt_core__DOT__zetas[106U] = 0x04c2U;
    vlSelfRef.ntt_core__DOT__zetas[107U] = 0xfac9U;
    vlSelfRef.ntt_core__DOT__zetas[108U] = 0xfc96U;
    vlSelfRef.ntt_core__DOT__zetas[109U] = 0x00dcU;
    vlSelfRef.ntt_core__DOT__zetas[110U] = 0xfb5dU;
    vlSelfRef.ntt_core__DOT__zetas[111U] = 0xf985U;
    vlSelfRef.ntt_core__DOT__zetas[112U] = 0xfb5fU;
    vlSelfRef.ntt_core__DOT__zetas[113U] = 0xfa06U;
    vlSelfRef.ntt_core__DOT__zetas[114U] = 0xfb02U;
    vlSelfRef.ntt_core__DOT__zetas[115U] = 0x031aU;
    vlSelfRef.ntt_core__DOT__zetas[116U] = 0xfa1aU;
    vlSelfRef.ntt_core__DOT__zetas[117U] = 0xfcaaU;
    vlSelfRef.ntt_core__DOT__zetas[118U] = 0xfc9aU;
    vlSelfRef.ntt_core__DOT__zetas[119U] = 0x01deU;
    vlSelfRef.ntt_core__DOT__zetas[120U] = 0xff94U;
    vlSelfRef.ntt_core__DOT__zetas[121U] = 0xfeccU;
    vlSelfRef.ntt_core__DOT__zetas[122U] = 0x03e4U;
    vlSelfRef.ntt_core__DOT__zetas[123U] = 0x03dfU;
    vlSelfRef.ntt_core__DOT__zetas[124U] = 0x03beU;
    vlSelfRef.ntt_core__DOT__zetas[125U] = 0xfa4cU;
    vlSelfRef.ntt_core__DOT__zetas[126U] = 0x05f2U;
    vlSelfRef.ntt_core__DOT__zetas[127U] = 0x065cU;
    ntt_core__DOT__u_mem__DOT__i = 0U;
    while (VL_GTS_III(32, 0x00000100U, ntt_core__DOT__u_mem__DOT__i)) {
        vlSelfRef.ntt_core__DOT__u_mem__DOT__mem[(0x000000ffU 
                                                  & ntt_core__DOT__u_mem__DOT__i)] = 0U;
        ntt_core__DOT__u_mem__DOT__i = ((IData)(1U) 
                                        + ntt_core__DOT__u_mem__DOT__i);
    }
}

VL_ATTR_COLD void Vntt_core___024root___eval_final(Vntt_core___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vntt_core___024root___eval_final\n"); );
    Vntt_core__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Vntt_core___024root___dump_triggers__stl(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag);
#endif  // VL_DEBUG
VL_ATTR_COLD bool Vntt_core___024root___eval_phase__stl(Vntt_core___024root* vlSelf);

VL_ATTR_COLD void Vntt_core___024root___eval_settle(Vntt_core___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vntt_core___024root___eval_settle\n"); );
    Vntt_core__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    IData/*31:0*/ __VstlIterCount;
    // Body
    __VstlIterCount = 0U;
    vlSelfRef.__VstlFirstIteration = 1U;
    do {
        if (VL_UNLIKELY(((0x00002710U < __VstlIterCount)))) {
#ifdef VL_DEBUG
            Vntt_core___024root___dump_triggers__stl(vlSelfRef.__VstlTriggered, "stl"s);
#endif
            VL_FATAL_MT("/Volumes/Work/projects/code/pqc-hsm-fpga/hardware/rtl/mlkem/ntt_core.v", 37, "", "DIDNOTCONVERGE: Settle region did not converge after '--converge-limit' of 10000 tries");
        }
        __VstlIterCount = ((IData)(1U) + __VstlIterCount);
        vlSelfRef.__VstlPhaseResult = Vntt_core___024root___eval_phase__stl(vlSelf);
        vlSelfRef.__VstlFirstIteration = 0U;
    } while (vlSelfRef.__VstlPhaseResult);
}

VL_ATTR_COLD bool Vntt_core___024root___trigger_anySet__stl(const VlUnpacked<QData/*63:0*/, 1> &in);

#ifdef VL_DEBUG
VL_ATTR_COLD void Vntt_core___024root___dump_triggers__stl(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vntt_core___024root___dump_triggers__stl\n"); );
    // Body
    if ((1U & (~ (IData)(Vntt_core___024root___trigger_anySet__stl(triggers))))) {
        VL_DBG_MSGS("         No '" + tag + "' region triggers active\n");
    }
    if ((1U & (IData)(triggers[0U]))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 0 is active: Internal 'stl' trigger - first iteration\n");
    }
}
#endif  // VL_DEBUG

VL_ATTR_COLD bool Vntt_core___024root___trigger_anySet__stl(const VlUnpacked<QData/*63:0*/, 1> &in) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vntt_core___024root___trigger_anySet__stl\n"); );
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

VL_ATTR_COLD void Vntt_core___024root___stl_sequent__TOP__0(Vntt_core___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vntt_core___024root___stl_sequent__TOP__0\n"); );
    Vntt_core__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    vlSelfRef.ntt_core__DOT__pb_we = 0U;
    vlSelfRef.rd_data = vlSelfRef.ntt_core__DOT__pb_dout;
    vlSelfRef.__VdfgRegularize_hebeb780c_0_0 = (((- (IData)(
                                                            (1U 
                                                             & ((IData)(vlSelfRef.ntt_core__DOT__pa_dout) 
                                                                >> 0x0000000fU)))) 
                                                 << 0x00000010U) 
                                                | (IData)(vlSelfRef.ntt_core__DOT__pa_dout));
    vlSelfRef.ntt_core__DOT__pa_we = 0U;
    vlSelfRef.ntt_core__DOT__u_bf_gs_h__DOT__sum = 
        (0x0000ffffU & ((IData)(vlSelfRef.ntt_core__DOT__pa_dout) 
                        + (IData)(vlSelfRef.ntt_core__DOT__pb_dout)));
    vlSelfRef.ntt_core__DOT__u_bf_gs_h__DOT__diff = 
        (0x0000ffffU & ((IData)(vlSelfRef.ntt_core__DOT__pb_dout) 
                        - (IData)(vlSelfRef.ntt_core__DOT__pa_dout)));
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
    vlSelfRef.ntt_core__DOT__pa_addr = 0U;
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
    vlSelfRef.ntt_core__DOT__pb_addr = 0U;
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
        if ((2U & (IData)(vlSelfRef.ntt_core__DOT__state))) {
            vlSelfRef.ntt_core__DOT__pa_addr = (0x000000ffU 
                                                & ((1U 
                                                    & (IData)(vlSelfRef.ntt_core__DOT__state))
                                                    ? (IData)(vlSelfRef.ntt_core__DOT__scale_i)
                                                    : (IData)(vlSelfRef.ntt_core__DOT__j)));
            if ((1U & (~ (IData)(vlSelfRef.ntt_core__DOT__state)))) {
                vlSelfRef.ntt_core__DOT__pb_addr = 
                    (0x000000ffU & (IData)(vlSelfRef.ntt_core__DOT__j_hi));
            }
        } else if ((1U & (~ (IData)(vlSelfRef.ntt_core__DOT__state)))) {
            vlSelfRef.ntt_core__DOT__pa_addr = (0x000000ffU 
                                                & (IData)(vlSelfRef.ntt_core__DOT__scale_i));
        }
    } else if ((2U & (IData)(vlSelfRef.ntt_core__DOT__state))) {
        if ((1U & (~ (IData)(vlSelfRef.ntt_core__DOT__state)))) {
            vlSelfRef.ntt_core__DOT__pa_we = 1U;
            vlSelfRef.ntt_core__DOT__pb_addr = (0x000000ffU 
                                                & (IData)(vlSelfRef.ntt_core__DOT__j_hi));
            vlSelfRef.ntt_core__DOT__pa_din = (0x0000ffffU 
                                               & ((IData)(vlSelfRef.ntt_core__DOT__inv_r)
                                                   ? (IData)(vlSelfRef.ntt_core__DOT__gs_a_r)
                                                   : 
                                                  ((IData)(vlSelfRef.ntt_core__DOT__bf_a_r) 
                                                   + (IData)(vlSelfRef.ntt_core__DOT__u_bf_ct_t__DOT__t))));
        }
        vlSelfRef.ntt_core__DOT__pa_addr = (0x000000ffU 
                                            & ((1U 
                                                & (IData)(vlSelfRef.ntt_core__DOT__state))
                                                ? (IData)(vlSelfRef.ntt_core__DOT__scale_i)
                                                : (IData)(vlSelfRef.ntt_core__DOT__j)));
    } else {
        if ((1U & (~ (IData)(vlSelfRef.ntt_core__DOT__state)))) {
            vlSelfRef.ntt_core__DOT__pa_we = vlSelfRef.wr_en;
            vlSelfRef.ntt_core__DOT__pa_din = vlSelfRef.wr_data;
        }
        if ((1U & (IData)(vlSelfRef.ntt_core__DOT__state))) {
            vlSelfRef.ntt_core__DOT__pa_addr = (0x000000ffU 
                                                & (IData)(vlSelfRef.ntt_core__DOT__j));
            vlSelfRef.ntt_core__DOT__pb_addr = (0x000000ffU 
                                                & (IData)(vlSelfRef.ntt_core__DOT__j_hi));
        } else {
            vlSelfRef.ntt_core__DOT__pa_addr = (0x000000ffU 
                                                & (IData)(vlSelfRef.wr_addr));
            vlSelfRef.ntt_core__DOT__pb_addr = (0x000000ffU 
                                                & (IData)(vlSelfRef.rd_addr));
        }
    }
}

VL_ATTR_COLD bool Vntt_core___024root___eval_phase__stl(Vntt_core___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vntt_core___024root___eval_phase__stl\n"); );
    Vntt_core__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
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
        Vntt_core___024root___dump_triggers__stl(vlSelfRef.__VstlTriggered, "stl"s);
    }
#endif
    __VstlExecute = Vntt_core___024root___trigger_anySet__stl(vlSelfRef.__VstlTriggered);
    if (__VstlExecute) {
        {
            // Inlined CFunc: _eval_stl
            if ((1ULL & vlSelfRef.__VstlTriggered[0U])) {
                Vntt_core___024root___stl_sequent__TOP__0(vlSelf);
            }
        }
    }
    return (__VstlExecute);
}

bool Vntt_core___024root___trigger_anySet__ico(const VlUnpacked<QData/*63:0*/, 2> &in);

#ifdef VL_DEBUG
VL_ATTR_COLD void Vntt_core___024root___dump_triggers__ico(const VlUnpacked<QData/*63:0*/, 2> &triggers, const std::string &tag) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vntt_core___024root___dump_triggers__ico\n"); );
    // Body
    if ((1U & (~ (IData)(Vntt_core___024root___trigger_anySet__ico(triggers))))) {
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
        VL_DBG_MSGS("         '" + tag + "' region trigger index 3 is active: @( inverse)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 4U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 4 is active: @( wr_en)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 5U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 5 is active: @( wr_addr)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 6U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 6 is active: @( wr_data)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 7U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 7 is active: @( rd_addr)\n");
    }
    if ((1U & (IData)(triggers[1U]))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 64 is active: Internal 'ico' trigger - first iteration\n");
    }
}
#endif  // VL_DEBUG

bool Vntt_core___024root___trigger_anySet__act(const VlUnpacked<QData/*63:0*/, 1> &in);

#ifdef VL_DEBUG
VL_ATTR_COLD void Vntt_core___024root___dump_triggers__act(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vntt_core___024root___dump_triggers__act\n"); );
    // Body
    if ((1U & (~ (IData)(Vntt_core___024root___trigger_anySet__act(triggers))))) {
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

VL_ATTR_COLD void Vntt_core___024root___ctor_var_reset(Vntt_core___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vntt_core___024root___ctor_var_reset\n"); );
    Vntt_core__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    const uint64_t __VscopeHash = VL_MURMUR64_HASH(vlSelf->vlNamep);
    vlSelf->clk = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 16707436170211756652ull);
    vlSelf->rst_n = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 1638864771569018232ull);
    vlSelf->start = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 9867861323841650631ull);
    vlSelf->inverse = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 1159024208945578495ull);
    vlSelf->done = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 10296494685231209730ull);
    vlSelf->wr_en = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 7710928637576349896ull);
    vlSelf->wr_addr = VL_SCOPED_RAND_RESET_I(8, __VscopeHash, 10458723662394441575ull);
    vlSelf->wr_data = VL_SCOPED_RAND_RESET_I(16, __VscopeHash, 12812822527505751231ull);
    vlSelf->rd_addr = VL_SCOPED_RAND_RESET_I(8, __VscopeHash, 7950012703377089919ull);
    vlSelf->rd_data = VL_SCOPED_RAND_RESET_I(16, __VscopeHash, 17824471296722538975ull);
    for (int __Vi0 = 0; __Vi0 < 128; ++__Vi0) {
        vlSelf->ntt_core__DOT__zetas[__Vi0] = VL_SCOPED_RAND_RESET_I(16, __VscopeHash, 16027226394388492581ull);
    }
    vlSelf->ntt_core__DOT__state = VL_SCOPED_RAND_RESET_I(3, __VscopeHash, 10808292630938228472ull);
    vlSelf->ntt_core__DOT__len = VL_SCOPED_RAND_RESET_I(9, __VscopeHash, 9657299378594676398ull);
    vlSelf->ntt_core__DOT__grp = VL_SCOPED_RAND_RESET_I(9, __VscopeHash, 12193357092288761294ull);
    vlSelf->ntt_core__DOT__j = VL_SCOPED_RAND_RESET_I(9, __VscopeHash, 341268640097399545ull);
    vlSelf->ntt_core__DOT__k = VL_SCOPED_RAND_RESET_I(8, __VscopeHash, 7168418755566510532ull);
    vlSelf->ntt_core__DOT__inv_r = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 17099626270176840984ull);
    vlSelf->ntt_core__DOT__scale_i = VL_SCOPED_RAND_RESET_I(9, __VscopeHash, 720647279667518593ull);
    vlSelf->ntt_core__DOT__j_hi = VL_SCOPED_RAND_RESET_I(9, __VscopeHash, 437647526198931431ull);
    vlSelf->ntt_core__DOT__pa_we = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 17566410321410877247ull);
    vlSelf->ntt_core__DOT__pb_we = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 728192272419128126ull);
    vlSelf->ntt_core__DOT__pa_addr = VL_SCOPED_RAND_RESET_I(8, __VscopeHash, 7130006300958516201ull);
    vlSelf->ntt_core__DOT__pb_addr = VL_SCOPED_RAND_RESET_I(8, __VscopeHash, 16155920140444916721ull);
    vlSelf->ntt_core__DOT__pa_din = VL_SCOPED_RAND_RESET_I(16, __VscopeHash, 15622378309800918758ull);
    vlSelf->ntt_core__DOT__pb_din = VL_SCOPED_RAND_RESET_I(16, __VscopeHash, 16942946652747090075ull);
    vlSelf->ntt_core__DOT__pa_dout = VL_SCOPED_RAND_RESET_I(16, __VscopeHash, 7104769338117026976ull);
    vlSelf->ntt_core__DOT__pb_dout = VL_SCOPED_RAND_RESET_I(16, __VscopeHash, 1475965944078388360ull);
    vlSelf->ntt_core__DOT__prod_r = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 17932666082115024019ull);
    vlSelf->ntt_core__DOT__bf_a_r = VL_SCOPED_RAND_RESET_I(16, __VscopeHash, 15600516822210772283ull);
    vlSelf->ntt_core__DOT__gs_a_r = VL_SCOPED_RAND_RESET_I(16, __VscopeHash, 9922280651973081078ull);
    vlSelf->ntt_core__DOT__scprod_r = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 12444560497932194007ull);
    vlSelf->ntt_core__DOT__scbarr_r = VL_SCOPED_RAND_RESET_I(16, __VscopeHash, 3031782006123213289ull);
    vlSelf->ntt_core__DOT__u_bf_ct_t__DOT__t = VL_SCOPED_RAND_RESET_I(16, __VscopeHash, 127240535776498274ull);
    vlSelf->ntt_core__DOT__u_bf_gs_h__DOT__sum = VL_SCOPED_RAND_RESET_I(16, __VscopeHash, 11823519107811331590ull);
    vlSelf->ntt_core__DOT__u_bf_gs_h__DOT__diff = VL_SCOPED_RAND_RESET_I(16, __VscopeHash, 13348369083770664671ull);
    for (int __Vi0 = 0; __Vi0 < 256; ++__Vi0) {
        vlSelf->ntt_core__DOT__u_mem__DOT__mem[__Vi0] = VL_SCOPED_RAND_RESET_I(16, __VscopeHash, 11000878554392752409ull);
    }
    vlSelf->__VdfgRegularize_hebeb780c_0_0 = 0;
    vlSelf->__VdfgRegularize_hebeb780c_0_1 = 0;
    vlSelf->__VdlyVal__ntt_core__DOT__u_mem__DOT__mem__v0 = 0;
    vlSelf->__VdlyDim0__ntt_core__DOT__u_mem__DOT__mem__v0 = 0;
    vlSelf->__VdlySet__ntt_core__DOT__u_mem__DOT__mem__v0 = 0;
    vlSelf->__VdlyVal__ntt_core__DOT__u_mem__DOT__mem__v1 = 0;
    vlSelf->__VdlyDim0__ntt_core__DOT__u_mem__DOT__mem__v1 = 0;
    vlSelf->__VdlySet__ntt_core__DOT__u_mem__DOT__mem__v1 = 0;
    for (int __Vi0 = 0; __Vi0 < 1; ++__Vi0) {
        vlSelf->__VstlTriggered[__Vi0] = 0;
    }
    for (int __Vi0 = 0; __Vi0 < 2; ++__Vi0) {
        vlSelf->__VicoTriggered[__Vi0] = 0;
    }
    vlSelf->__Vtrigprevexpr___TOP__clk__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__rst_n__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__start__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__inverse__0 = 0;
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
