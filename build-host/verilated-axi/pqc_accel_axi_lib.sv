// DESCRIPTION: Verilator generated Verilog
// Wrapper module for DPI protected library
// This module requires libpqc_accel_axi_lib.a or libpqc_accel_axi_lib.so to work
// See instructions in your simulator for how to use DPI libraries

module pqc_accel_axi_lib (
        input logic clk
        , input logic rst_n
        , input logic [7:0]  s_axi_awaddr
        , input logic s_axi_awvalid
        , output logic s_axi_awready
        , input logic [3:0]  s_axi_wstrb
        , input logic s_axi_wvalid
        , output logic s_axi_wready
        , output logic [1:0]  s_axi_bresp
        , output logic s_axi_bvalid
        , input logic s_axi_bready
        , input logic [7:0]  s_axi_araddr
        , input logic s_axi_arvalid
        , output logic s_axi_arready
        , output logic [1:0]  s_axi_rresp
        , output logic s_axi_rvalid
        , input logic s_axi_rready
        , input logic s_axis_tvalid
        , output logic s_axis_tready
        , input logic s_axis_tlast
        , output logic m_axis_tvalid
        , input logic m_axis_tready
        , output logic m_axis_tlast
        , input logic [31:0]  s_axi_wdata
        , output logic [31:0]  s_axi_rdata
        , input logic [31:0]  s_axis_tdata
        , output logic [31:0]  m_axis_tdata
    );
    
    // Precision of submodule (commented out to avoid requiring timescale on all modules)
    // timeunit 1ps;
    // timeprecision 1ps;
    
    // Checks to make sure the .sv wrapper and library agree
    import "DPI-C" function void pqc_accel_axi_lib_protectlib_check_hash(int protectlib_hash__V);
    
    // Creates an instance of the library module at initial-time
    // (one for each instance in the user's design) also evaluates
    // the library module's initial process
    import "DPI-C" function chandle pqc_accel_axi_lib_protectlib_create(string scope__V);
    
    // Updates all non-clock inputs and retrieves the results
    import "DPI-C" function longint pqc_accel_axi_lib_protectlib_combo_update(
        chandle handle__V
        , input logic [7:0]  s_axi_awaddr
        , input logic s_axi_awvalid
        , output logic s_axi_awready
        , input logic [3:0]  s_axi_wstrb
        , input logic s_axi_wvalid
        , output logic s_axi_wready
        , output logic [1:0]  s_axi_bresp
        , output logic s_axi_bvalid
        , input logic s_axi_bready
        , input logic [7:0]  s_axi_araddr
        , input logic s_axi_arvalid
        , output logic s_axi_arready
        , output logic [1:0]  s_axi_rresp
        , output logic s_axi_rvalid
        , input logic s_axi_rready
        , input logic s_axis_tvalid
        , output logic s_axis_tready
        , input logic s_axis_tlast
        , output logic m_axis_tvalid
        , input logic m_axis_tready
        , output logic m_axis_tlast
        , input logic [31:0]  s_axi_wdata
        , output logic [31:0]  s_axi_rdata
        , input logic [31:0]  s_axis_tdata
        , output logic [31:0]  m_axis_tdata
    );
    
    // Updates all clocks and retrieves the results
    import "DPI-C" function longint pqc_accel_axi_lib_protectlib_seq_update(
        chandle handle__V
        , input logic clk
        , input logic rst_n
        , output logic s_axi_awready
        , output logic s_axi_wready
        , output logic [1:0]  s_axi_bresp
        , output logic s_axi_bvalid
        , output logic s_axi_arready
        , output logic [1:0]  s_axi_rresp
        , output logic s_axi_rvalid
        , output logic s_axis_tready
        , output logic m_axis_tvalid
        , output logic m_axis_tlast
        , output logic [31:0]  s_axi_rdata
        , output logic [31:0]  m_axis_tdata
    );
    
    // Need to convince some simulators that the input to the module
    // must be evaluated before evaluating the clock edge
    import "DPI-C" function void pqc_accel_axi_lib_protectlib_combo_ignore(
        chandle handle__V
        , input logic [7:0]  s_axi_awaddr
        , input logic s_axi_awvalid
        , input logic [3:0]  s_axi_wstrb
        , input logic s_axi_wvalid
        , input logic s_axi_bready
        , input logic [7:0]  s_axi_araddr
        , input logic s_axi_arvalid
        , input logic s_axi_rready
        , input logic s_axis_tvalid
        , input logic s_axis_tlast
        , input logic m_axis_tready
        , input logic [31:0]  s_axi_wdata
        , input logic [31:0]  s_axis_tdata
    );
    
    // Evaluates the library module's final process
    import "DPI-C" function void pqc_accel_axi_lib_protectlib_final(chandle handle__V);
    
    // verilator tracing_off
    chandle handle__V;
    time last_combo_seqnum__V;
    time last_seq_seqnum__V;

    logic s_axi_awready_combo__V;
    logic s_axi_wready_combo__V;
    logic [1:0]  s_axi_bresp_combo__V;
    logic s_axi_bvalid_combo__V;
    logic s_axi_arready_combo__V;
    logic [1:0]  s_axi_rresp_combo__V;
    logic s_axi_rvalid_combo__V;
    logic s_axis_tready_combo__V;
    logic m_axis_tvalid_combo__V;
    logic m_axis_tlast_combo__V;
    logic [31:0]  s_axi_rdata_combo__V;
    logic [31:0]  m_axis_tdata_combo__V;
    logic s_axi_awready_seq__V;
    logic s_axi_wready_seq__V;
    logic [1:0]  s_axi_bresp_seq__V;
    logic s_axi_bvalid_seq__V;
    logic s_axi_arready_seq__V;
    logic [1:0]  s_axi_rresp_seq__V;
    logic s_axi_rvalid_seq__V;
    logic s_axis_tready_seq__V;
    logic m_axis_tvalid_seq__V;
    logic m_axis_tlast_seq__V;
    logic [31:0]  s_axi_rdata_seq__V;
    logic [31:0]  m_axis_tdata_seq__V;
    logic s_axi_awready_tmp__V;
    logic s_axi_wready_tmp__V;
    logic [1:0]  s_axi_bresp_tmp__V;
    logic s_axi_bvalid_tmp__V;
    logic s_axi_arready_tmp__V;
    logic [1:0]  s_axi_rresp_tmp__V;
    logic s_axi_rvalid_tmp__V;
    logic s_axis_tready_tmp__V;
    logic m_axis_tvalid_tmp__V;
    logic m_axis_tlast_tmp__V;
    logic [31:0]  s_axi_rdata_tmp__V;
    logic [31:0]  m_axis_tdata_tmp__V;
    // Hash value to make sure this file and the corresponding
    // library agree
    localparam int protectlib_hash__V = 32'd3097583484;

    initial begin
        pqc_accel_axi_lib_protectlib_check_hash(protectlib_hash__V);
        handle__V = pqc_accel_axi_lib_protectlib_create($sformatf("%m"));
    end
    
    // Combinatorially evaluate changes to inputs
    always_comb begin
        last_combo_seqnum__V = pqc_accel_axi_lib_protectlib_combo_update(
            handle__V,
            s_axi_awaddr,
            s_axi_awvalid,
            s_axi_awready_combo__V,
            s_axi_wstrb,
            s_axi_wvalid,
            s_axi_wready_combo__V,
            s_axi_bresp_combo__V,
            s_axi_bvalid_combo__V,
            s_axi_bready,
            s_axi_araddr,
            s_axi_arvalid,
            s_axi_arready_combo__V,
            s_axi_rresp_combo__V,
            s_axi_rvalid_combo__V,
            s_axi_rready,
            s_axis_tvalid,
            s_axis_tready_combo__V,
            s_axis_tlast,
            m_axis_tvalid_combo__V,
            m_axis_tready,
            m_axis_tlast_combo__V,
            s_axi_wdata,
            s_axi_rdata_combo__V,
            s_axis_tdata,
            m_axis_tdata_combo__V
        );
    end
    
    // Evaluate clock edges
    always @(clk or rst_n) begin
        pqc_accel_axi_lib_protectlib_combo_ignore(
            handle__V,
            s_axi_awaddr,
            s_axi_awvalid,
            s_axi_wstrb,
            s_axi_wvalid,
            s_axi_bready,
            s_axi_araddr,
            s_axi_arvalid,
            s_axi_rready,
            s_axis_tvalid,
            s_axis_tlast,
            m_axis_tready,
            s_axi_wdata,
            s_axis_tdata
        );
        last_seq_seqnum__V <= pqc_accel_axi_lib_protectlib_seq_update(
            handle__V,
            clk,
            rst_n,
            s_axi_awready_tmp__V,
            s_axi_wready_tmp__V,
            s_axi_bresp_tmp__V,
            s_axi_bvalid_tmp__V,
            s_axi_arready_tmp__V,
            s_axi_rresp_tmp__V,
            s_axi_rvalid_tmp__V,
            s_axis_tready_tmp__V,
            m_axis_tvalid_tmp__V,
            m_axis_tlast_tmp__V,
            s_axi_rdata_tmp__V,
            m_axis_tdata_tmp__V
        );
        s_axi_awready_seq__V <= s_axi_awready_tmp__V;
        s_axi_wready_seq__V <= s_axi_wready_tmp__V;
        s_axi_bresp_seq__V <= s_axi_bresp_tmp__V;
        s_axi_bvalid_seq__V <= s_axi_bvalid_tmp__V;
        s_axi_arready_seq__V <= s_axi_arready_tmp__V;
        s_axi_rresp_seq__V <= s_axi_rresp_tmp__V;
        s_axi_rvalid_seq__V <= s_axi_rvalid_tmp__V;
        s_axis_tready_seq__V <= s_axis_tready_tmp__V;
        m_axis_tvalid_seq__V <= m_axis_tvalid_tmp__V;
        m_axis_tlast_seq__V <= m_axis_tlast_tmp__V;
        s_axi_rdata_seq__V <= s_axi_rdata_tmp__V;
        m_axis_tdata_seq__V <= m_axis_tdata_tmp__V;
    end
    
    // Select between combinatorial and sequential results
    always_comb begin
        if (last_seq_seqnum__V > last_combo_seqnum__V) begin
            s_axi_awready = s_axi_awready_seq__V;
            s_axi_wready = s_axi_wready_seq__V;
            s_axi_bresp = s_axi_bresp_seq__V;
            s_axi_bvalid = s_axi_bvalid_seq__V;
            s_axi_arready = s_axi_arready_seq__V;
            s_axi_rresp = s_axi_rresp_seq__V;
            s_axi_rvalid = s_axi_rvalid_seq__V;
            s_axis_tready = s_axis_tready_seq__V;
            m_axis_tvalid = m_axis_tvalid_seq__V;
            m_axis_tlast = m_axis_tlast_seq__V;
            s_axi_rdata = s_axi_rdata_seq__V;
            m_axis_tdata = m_axis_tdata_seq__V;
        end else begin
            s_axi_awready = s_axi_awready_combo__V;
            s_axi_wready = s_axi_wready_combo__V;
            s_axi_bresp = s_axi_bresp_combo__V;
            s_axi_bvalid = s_axi_bvalid_combo__V;
            s_axi_arready = s_axi_arready_combo__V;
            s_axi_rresp = s_axi_rresp_combo__V;
            s_axi_rvalid = s_axi_rvalid_combo__V;
            s_axis_tready = s_axis_tready_combo__V;
            m_axis_tvalid = m_axis_tvalid_combo__V;
            m_axis_tlast = m_axis_tlast_combo__V;
            s_axi_rdata = s_axi_rdata_combo__V;
            m_axis_tdata = m_axis_tdata_combo__V;
        end
    end
    
    final pqc_accel_axi_lib_protectlib_final(handle__V);
    
endmodule

`ifdef VERILATOR
`verilator_config
verilator_lib -module "pqc_accel_axi_lib"
profile_data -hier-dpi "pqc_accel_axi_lib_protectlib_combo_update" -cost 64'd0
profile_data -hier-dpi "pqc_accel_axi_lib_protectlib_seq_update" -cost 64'd0
profile_data -hier-dpi "pqc_accel_axi_lib_protectlib_combo_ignore" -cost 64'd1
hier_workers -hier-dpi "pqc_accel_axi_lib_protectlib_combo_update" -workers 16'd0
hier_workers -hier-dpi "pqc_accel_axi_lib_protectlib_seq_update" -workers 16'd0
`verilog
`endif
