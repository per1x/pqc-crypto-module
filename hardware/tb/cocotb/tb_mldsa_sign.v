// 把 mldsa_sign 接上一个 sha3_core，供 cocotb 当顶层用（增量验证）
`default_nettype none
module tb_mldsa_sign (
    input  wire clk, input wire rst_n,
    input  wire        start,
    input  wire        sk_wr_en,
    input  wire [11:0] sk_wr_addr,
    input  wire [7:0]  sk_wr_data,
    input  wire        msg_wr_en,
    input  wire [12:0] msg_wr_addr,
    input  wire [7:0]  msg_wr_data,
    input  wire        ctx_wr_en,
    input  wire [7:0]  ctx_wr_addr,
    input  wire [7:0]  ctx_wr_data,
    input  wire [13:0] msg_len,
    input  wire [7:0]  ctx_len,
    input  wire [255:0] rnd,
    output wire        done,
    output wire [255:0] rho,
    output wire [255:0] key_out,
    output wire [511:0] tr_out,
    output wire [511:0] mu,
    output wire [511:0] rhopp,
    input  wire [4:0]  dbg_sel,
    input  wire [7:0]  dbg_idx,
    output wire signed [31:0] dbg_coef,
    input  wire [11:0] sig_addr,
    output wire [7:0]  sig_data
);
    wire        ss, siv, sif, sor;
    wire [7:0]  sr, su, sid, sod;
    wire        sir, sov;

    mldsa_sign u_sign (
        .clk(clk), .rst_n(rst_n), .start(start),
        .sk_wr_en(sk_wr_en), .sk_wr_addr(sk_wr_addr), .sk_wr_data(sk_wr_data),
        .msg_wr_en(msg_wr_en), .msg_wr_addr(msg_wr_addr), .msg_wr_data(msg_wr_data),
        .ctx_wr_en(ctx_wr_en), .ctx_wr_addr(ctx_wr_addr), .ctx_wr_data(ctx_wr_data),
        .msg_len(msg_len), .ctx_len(ctx_len), .rnd(rnd),
        .done(done),
        .sha_start(ss), .sha_rate(sr), .sha_suffix(su),
        .sha_in_valid(siv), .sha_in_data(sid), .sha_in_flush(sif),
        .sha_in_ready(sir), .sha_out_valid(sov), .sha_out_ready(sor),
        .sha_out_data(sod),
        .rho(rho), .key_out(key_out), .tr_out(tr_out),
        .mu(mu), .rhopp(rhopp),
        .dbg_sel(dbg_sel), .dbg_idx(dbg_idx), .dbg_coef(dbg_coef),
        .sig_addr(sig_addr), .sig_data(sig_data));

    sha3_core u_sha (
        .clk(clk), .rst_n(rst_n), .rate_bytes(sr), .suffix(su),
        .start(ss), .zeroize(1'b0),
        .in_valid(siv), .in_ready(sir), .in_data(sid), .in_flush(sif),
        .out_valid(sov), .out_ready(sor), .out_data(sod),
        .busy(), .absorbing(), .squeezing(),
        .ext_start(1'b0), .ext_done(), .ext_wr_en(1'b0),
        .ext_wr_addr(5'd0), .ext_wr_data(64'd0), .ext_rd_addr(5'd0), .ext_rd_data());
endmodule
`default_nettype wire
