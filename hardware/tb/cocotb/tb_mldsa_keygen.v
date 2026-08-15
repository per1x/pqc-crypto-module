// 把 mldsa_keygen 接上一个 sha3_core，供 cocotb 当顶层用（增量验证）
`default_nettype none
module tb_mldsa_keygen (
    input  wire clk, input  wire rst_n,
    input  wire        start,
    input  wire [255:0] xi,
    output wire        done,
    output wire [255:0] rho,
    output wire [511:0] rho_prime,
    output wire [255:0] key_out,
    input  wire [3:0]  dbg_sel,
    input  wire [7:0]  dbg_idx,
    output wire signed [31:0] dbg_coef,
    input  wire [11:0] pk_addr,
    output wire [7:0]  pk_data,
    input  wire [11:0] sk_addr,
    output wire [7:0]  sk_data
);
    wire        ss, siv, sif, sor;
    wire [7:0]  sr, su, sid, sod;
    wire        sir, sov;

    mldsa_keygen u_kg (
        .clk(clk), .rst_n(rst_n), .start(start), .xi(xi), .done(done),
        .sha_start(ss), .sha_rate(sr), .sha_suffix(su),
        .sha_in_valid(siv), .sha_in_data(sid), .sha_in_flush(sif),
        .sha_in_ready(sir), .sha_out_valid(sov), .sha_out_ready(sor),
        .sha_out_data(sod),
        .rho(rho), .rho_prime(rho_prime), .key_out(key_out),
        .dbg_sel(dbg_sel), .dbg_idx(dbg_idx), .dbg_coef(dbg_coef),
        .pk_addr(pk_addr), .pk_data(pk_data),
        .sk_addr(sk_addr), .sk_data(sk_data));

    sha3_core u_sha (
        .clk(clk), .rst_n(rst_n), .rate_bytes(sr), .suffix(su),
        .start(ss), .zeroize(1'b0),
        .in_valid(siv), .in_ready(sir), .in_data(sid), .in_flush(sif),
        .out_valid(sov), .out_ready(sor), .out_data(sod),
        .busy(), .absorbing(), .squeezing(),
        .ext_start(1'b0), .ext_done(), .ext_wr_en(1'b0),
        .ext_wr_addr(5'd0), .ext_wr_data(64'd0), .ext_rd_addr(5'd0), .ext_rd_data());
endmodule
