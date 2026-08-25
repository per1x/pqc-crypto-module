// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Generated C++
// Wrapper functions for DPI protected library

#include "Vntt_core.h"
#include "verilated_dpi.h"

#include <cstdio>
#include <cstdlib>

// Container class to house verilated object and sequence number
class Vntt_core_container: public Vntt_core {
  public:
    long long m_seqnum;
    Vntt_core_container(const char* scopep__V):
    Vntt_core(scopep__V) {}
};

extern "C" {
    
    // Checks to make sure the .sv wrapper and library agree
    void ntt_core_lib_protectlib_check_hash(int protectlib_hash__V) {
        const int expected_hash__V = 244217150U;
        if (protectlib_hash__V != expected_hash__V) {
            fprintf(stderr, "%%Error: cannot use ntt_core_lib library, Verliog (%u) and library (%u) hash values do not agree\n", protectlib_hash__V, expected_hash__V);
            std::exit(EXIT_FAILURE);
        }
    }
    
    // Creates an instance of the library module at initial-time
    // (one for each instance in the user's design) also evaluates
    // the library module's initial process
    void* ntt_core_lib_protectlib_create(const char* scopep__V) {
        Vntt_core_container* const handlep__V = new Vntt_core_container{scopep__V};
        return handlep__V;
    }
    
    // Updates all non-clock inputs and retrieves the results
    long long ntt_core_lib_protectlib_combo_update(
        void* vhandlep__V,
        svLogic start,
        svLogic inverse,
        svLogic* done,
        svLogic wr_en,
        const svLogicVecVal* wr_addr,
        const svLogicVecVal* rd_addr,
        const svLogicVecVal* wr_data,
        svLogicVecVal* rd_data
    ) {
        Vntt_core_container* const handlep__V = static_cast<Vntt_core_container*>(vhandlep__V);
        handlep__V->start = (start);
        handlep__V->inverse = (inverse);
        handlep__V->wr_en = (wr_en);
        VL_SET_C_SVLV(8,  handlep__V->wr_addr, wr_addr + 0);
        VL_SET_C_SVLV(8,  handlep__V->rd_addr, rd_addr + 0);
        VL_SET_S_SVLV(16,  handlep__V->wr_data, wr_data + 0);
        handlep__V->eval();
        *done = handlep__V->done;
        VL_SET_SVLV_I(16, rd_data, handlep__V->rd_data);
        return handlep__V->m_seqnum++;
    }
    
    // Updates all clocks and retrieves the results
    long long ntt_core_lib_protectlib_seq_update(
        void* vhandlep__V,
        svLogic clk,
        svLogic rst_n,
        svLogic* done,
        svLogicVecVal* rd_data
    ) {
        Vntt_core_container* const handlep__V = static_cast<Vntt_core_container*>(vhandlep__V);
        handlep__V->clk = (clk);
        handlep__V->rst_n = (rst_n);
        handlep__V->eval();
        *done = handlep__V->done;
        VL_SET_SVLV_I(16, rd_data, handlep__V->rd_data);
        return handlep__V->m_seqnum++;
    }
    
    // Need to convince some simulators that the input to the module
    // must be evaluated before evaluating the clock edge
    void ntt_core_lib_protectlib_combo_ignore(
        void* vhandlep__V,
        svLogic start,
        svLogic inverse,
        svLogic wr_en,
        const svLogicVecVal* wr_addr,
        const svLogicVecVal* rd_addr,
        const svLogicVecVal* wr_data
    )
    { }
    
    // Evaluates the library module's final process
    void ntt_core_lib_protectlib_final(void* vhandlep__V) {
        Vntt_core_container* const handlep__V = static_cast<Vntt_core_container*>(vhandlep__V);
        handlep__V->final();
        delete handlep__V;
    }
    
}
