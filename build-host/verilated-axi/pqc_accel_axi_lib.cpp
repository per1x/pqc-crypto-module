// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Generated C++
// Wrapper functions for DPI protected library

#include "Vpqc_accel_axi.h"
#include "verilated_dpi.h"

#include <cstdio>
#include <cstdlib>

// Container class to house verilated object and sequence number
class Vpqc_accel_axi_container: public Vpqc_accel_axi {
  public:
    long long m_seqnum;
    Vpqc_accel_axi_container(const char* scopep__V):
    Vpqc_accel_axi(scopep__V) {}
};

extern "C" {
    
    // Checks to make sure the .sv wrapper and library agree
    void pqc_accel_axi_lib_protectlib_check_hash(int protectlib_hash__V) {
        const int expected_hash__V = 3097583484U;
        if (protectlib_hash__V != expected_hash__V) {
            fprintf(stderr, "%%Error: cannot use pqc_accel_axi_lib library, Verliog (%u) and library (%u) hash values do not agree\n", protectlib_hash__V, expected_hash__V);
            std::exit(EXIT_FAILURE);
        }
    }
    
    // Creates an instance of the library module at initial-time
    // (one for each instance in the user's design) also evaluates
    // the library module's initial process
    void* pqc_accel_axi_lib_protectlib_create(const char* scopep__V) {
        Vpqc_accel_axi_container* const handlep__V = new Vpqc_accel_axi_container{scopep__V};
        return handlep__V;
    }
    
    // Updates all non-clock inputs and retrieves the results
    long long pqc_accel_axi_lib_protectlib_combo_update(
        void* vhandlep__V,
        const svLogicVecVal* s_axi_awaddr,
        svLogic s_axi_awvalid,
        svLogic* s_axi_awready,
        const svLogicVecVal* s_axi_wstrb,
        svLogic s_axi_wvalid,
        svLogic* s_axi_wready,
        svLogicVecVal* s_axi_bresp,
        svLogic* s_axi_bvalid,
        svLogic s_axi_bready,
        const svLogicVecVal* s_axi_araddr,
        svLogic s_axi_arvalid,
        svLogic* s_axi_arready,
        svLogicVecVal* s_axi_rresp,
        svLogic* s_axi_rvalid,
        svLogic s_axi_rready,
        svLogic s_axis_tvalid,
        svLogic* s_axis_tready,
        svLogic s_axis_tlast,
        svLogic* m_axis_tvalid,
        svLogic m_axis_tready,
        svLogic* m_axis_tlast,
        const svLogicVecVal* s_axi_wdata,
        svLogicVecVal* s_axi_rdata,
        const svLogicVecVal* s_axis_tdata,
        svLogicVecVal* m_axis_tdata
    ) {
        Vpqc_accel_axi_container* const handlep__V = static_cast<Vpqc_accel_axi_container*>(vhandlep__V);
        VL_SET_C_SVLV(8,  handlep__V->s_axi_awaddr, s_axi_awaddr + 0);
        handlep__V->s_axi_awvalid = (s_axi_awvalid);
        VL_SET_C_SVLV(4,  handlep__V->s_axi_wstrb, s_axi_wstrb + 0);
        handlep__V->s_axi_wvalid = (s_axi_wvalid);
        handlep__V->s_axi_bready = (s_axi_bready);
        VL_SET_C_SVLV(8,  handlep__V->s_axi_araddr, s_axi_araddr + 0);
        handlep__V->s_axi_arvalid = (s_axi_arvalid);
        handlep__V->s_axi_rready = (s_axi_rready);
        handlep__V->s_axis_tvalid = (s_axis_tvalid);
        handlep__V->s_axis_tlast = (s_axis_tlast);
        handlep__V->m_axis_tready = (m_axis_tready);
        VL_SET_I_SVLV(32,  handlep__V->s_axi_wdata, s_axi_wdata + 0);
        VL_SET_I_SVLV(32,  handlep__V->s_axis_tdata, s_axis_tdata + 0);
        handlep__V->eval();
        *s_axi_awready = handlep__V->s_axi_awready;
        *s_axi_wready = handlep__V->s_axi_wready;
        VL_SET_SVLV_I(2, s_axi_bresp, handlep__V->s_axi_bresp);
        *s_axi_bvalid = handlep__V->s_axi_bvalid;
        *s_axi_arready = handlep__V->s_axi_arready;
        VL_SET_SVLV_I(2, s_axi_rresp, handlep__V->s_axi_rresp);
        *s_axi_rvalid = handlep__V->s_axi_rvalid;
        *s_axis_tready = handlep__V->s_axis_tready;
        *m_axis_tvalid = handlep__V->m_axis_tvalid;
        *m_axis_tlast = handlep__V->m_axis_tlast;
        VL_SET_SVLV_I(32, s_axi_rdata, handlep__V->s_axi_rdata);
        VL_SET_SVLV_I(32, m_axis_tdata, handlep__V->m_axis_tdata);
        return handlep__V->m_seqnum++;
    }
    
    // Updates all clocks and retrieves the results
    long long pqc_accel_axi_lib_protectlib_seq_update(
        void* vhandlep__V,
        svLogic clk,
        svLogic rst_n,
        svLogic* s_axi_awready,
        svLogic* s_axi_wready,
        svLogicVecVal* s_axi_bresp,
        svLogic* s_axi_bvalid,
        svLogic* s_axi_arready,
        svLogicVecVal* s_axi_rresp,
        svLogic* s_axi_rvalid,
        svLogic* s_axis_tready,
        svLogic* m_axis_tvalid,
        svLogic* m_axis_tlast,
        svLogicVecVal* s_axi_rdata,
        svLogicVecVal* m_axis_tdata
    ) {
        Vpqc_accel_axi_container* const handlep__V = static_cast<Vpqc_accel_axi_container*>(vhandlep__V);
        handlep__V->clk = (clk);
        handlep__V->rst_n = (rst_n);
        handlep__V->eval();
        *s_axi_awready = handlep__V->s_axi_awready;
        *s_axi_wready = handlep__V->s_axi_wready;
        VL_SET_SVLV_I(2, s_axi_bresp, handlep__V->s_axi_bresp);
        *s_axi_bvalid = handlep__V->s_axi_bvalid;
        *s_axi_arready = handlep__V->s_axi_arready;
        VL_SET_SVLV_I(2, s_axi_rresp, handlep__V->s_axi_rresp);
        *s_axi_rvalid = handlep__V->s_axi_rvalid;
        *s_axis_tready = handlep__V->s_axis_tready;
        *m_axis_tvalid = handlep__V->m_axis_tvalid;
        *m_axis_tlast = handlep__V->m_axis_tlast;
        VL_SET_SVLV_I(32, s_axi_rdata, handlep__V->s_axi_rdata);
        VL_SET_SVLV_I(32, m_axis_tdata, handlep__V->m_axis_tdata);
        return handlep__V->m_seqnum++;
    }
    
    // Need to convince some simulators that the input to the module
    // must be evaluated before evaluating the clock edge
    void pqc_accel_axi_lib_protectlib_combo_ignore(
        void* vhandlep__V,
        const svLogicVecVal* s_axi_awaddr,
        svLogic s_axi_awvalid,
        const svLogicVecVal* s_axi_wstrb,
        svLogic s_axi_wvalid,
        svLogic s_axi_bready,
        const svLogicVecVal* s_axi_araddr,
        svLogic s_axi_arvalid,
        svLogic s_axi_rready,
        svLogic s_axis_tvalid,
        svLogic s_axis_tlast,
        svLogic m_axis_tready,
        const svLogicVecVal* s_axi_wdata,
        const svLogicVecVal* s_axis_tdata
    )
    { }
    
    // Evaluates the library module's final process
    void pqc_accel_axi_lib_protectlib_final(void* vhandlep__V) {
        Vpqc_accel_axi_container* const handlep__V = static_cast<Vpqc_accel_axi_container*>(vhandlep__V);
        handlep__V->final();
        delete handlep__V;
    }
    
}
