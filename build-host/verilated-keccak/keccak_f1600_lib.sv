// DESCRIPTION: Verilator generated Verilog
// Wrapper module for DPI protected library
// This module requires libkeccak_f1600_lib.a or libkeccak_f1600_lib.so to work
// See instructions in your simulator for how to use DPI libraries

module keccak_f1600_lib (
        input logic clk
        , input logic rst_n
        , input logic start
        , output logic done
        , input logic wr_en
        , input logic [4:0]  wr_addr
        , input logic [4:0]  rd_addr
        , input logic [63:0]  wr_data
        , output logic [63:0]  rd_data
    );
    
    // Precision of submodule (commented out to avoid requiring timescale on all modules)
    // timeunit 1ps;
    // timeprecision 1ps;
    
    // Checks to make sure the .sv wrapper and library agree
    import "DPI-C" function void keccak_f1600_lib_protectlib_check_hash(int protectlib_hash__V);
    
    // Creates an instance of the library module at initial-time
    // (one for each instance in the user's design) also evaluates
    // the library module's initial process
    import "DPI-C" function chandle keccak_f1600_lib_protectlib_create(string scope__V);
    
    // Updates all non-clock inputs and retrieves the results
    import "DPI-C" function longint keccak_f1600_lib_protectlib_combo_update(
        chandle handle__V
        , input logic start
        , output logic done
        , input logic wr_en
        , input logic [4:0]  wr_addr
        , input logic [4:0]  rd_addr
        , input logic [63:0]  wr_data
        , output logic [63:0]  rd_data
    );
    
    // Updates all clocks and retrieves the results
    import "DPI-C" function longint keccak_f1600_lib_protectlib_seq_update(
        chandle handle__V
        , input logic clk
        , input logic rst_n
        , output logic done
        , output logic [63:0]  rd_data
    );
    
    // Need to convince some simulators that the input to the module
    // must be evaluated before evaluating the clock edge
    import "DPI-C" function void keccak_f1600_lib_protectlib_combo_ignore(
        chandle handle__V
        , input logic start
        , input logic wr_en
        , input logic [4:0]  wr_addr
        , input logic [4:0]  rd_addr
        , input logic [63:0]  wr_data
    );
    
    // Evaluates the library module's final process
    import "DPI-C" function void keccak_f1600_lib_protectlib_final(chandle handle__V);
    
    // verilator tracing_off
    chandle handle__V;
    time last_combo_seqnum__V;
    time last_seq_seqnum__V;

    logic done_combo__V;
    logic [63:0]  rd_data_combo__V;
    logic done_seq__V;
    logic [63:0]  rd_data_seq__V;
    logic done_tmp__V;
    logic [63:0]  rd_data_tmp__V;
    // Hash value to make sure this file and the corresponding
    // library agree
    localparam int protectlib_hash__V = 32'd2580908002;

    initial begin
        keccak_f1600_lib_protectlib_check_hash(protectlib_hash__V);
        handle__V = keccak_f1600_lib_protectlib_create($sformatf("%m"));
    end
    
    // Combinatorially evaluate changes to inputs
    always_comb begin
        last_combo_seqnum__V = keccak_f1600_lib_protectlib_combo_update(
            handle__V,
            start,
            done_combo__V,
            wr_en,
            wr_addr,
            rd_addr,
            wr_data,
            rd_data_combo__V
        );
    end
    
    // Evaluate clock edges
    always @(clk or rst_n) begin
        keccak_f1600_lib_protectlib_combo_ignore(
            handle__V,
            start,
            wr_en,
            wr_addr,
            rd_addr,
            wr_data
        );
        last_seq_seqnum__V <= keccak_f1600_lib_protectlib_seq_update(
            handle__V,
            clk,
            rst_n,
            done_tmp__V,
            rd_data_tmp__V
        );
        done_seq__V <= done_tmp__V;
        rd_data_seq__V <= rd_data_tmp__V;
    end
    
    // Select between combinatorial and sequential results
    always_comb begin
        if (last_seq_seqnum__V > last_combo_seqnum__V) begin
            done = done_seq__V;
            rd_data = rd_data_seq__V;
        end else begin
            done = done_combo__V;
            rd_data = rd_data_combo__V;
        end
    end
    
    final keccak_f1600_lib_protectlib_final(handle__V);
    
endmodule

`ifdef VERILATOR
`verilator_config
verilator_lib -module "keccak_f1600_lib"
profile_data -hier-dpi "keccak_f1600_lib_protectlib_combo_update" -cost 64'd0
profile_data -hier-dpi "keccak_f1600_lib_protectlib_seq_update" -cost 64'd0
profile_data -hier-dpi "keccak_f1600_lib_protectlib_combo_ignore" -cost 64'd1
hier_workers -hier-dpi "keccak_f1600_lib_protectlib_combo_update" -workers 16'd0
hier_workers -hier-dpi "keccak_f1600_lib_protectlib_seq_update" -workers 16'd0
`verilog
`endif
