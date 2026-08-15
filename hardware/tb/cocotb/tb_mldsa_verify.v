// 把 mldsa_verify 接上一个 sha3_core，供 cocotb 当顶层用（增量验证）
`default_nettype none
module tb_mldsa_verify #(
    parameter integer K    = 4,
    parameter integer L    = 4,
    parameter integer TAU  = 39,
    parameter integer G1LOG = 17,
    parameter integer MODE = 0,
    parameter integer OMG  = 80,
    parameter integer BETA = 78,
    parameter integer CTB  = 32
) (
    input  wire clk, input wire rst_n,
    input  wire        start,
    input  wire        pk_wr_en,
    input  wire [12:0] pk_wr_addr,
    input  wire [7:0]  pk_wr_data,
    input  wire        sig_wr_en,
    input  wire [12:0] sig_wr_addr,
    input  wire [7:0]  sig_wr_data,
    input  wire        msg_wr_en,
    input  wire [12:0] msg_wr_addr,
    input  wire [7:0]  msg_wr_data,
    input  wire        ctx_wr_en,
    input  wire [7:0]  ctx_wr_addr,
    input  wire [7:0]  ctx_wr_data,
    input  wire [13:0] msg_len,
    input  wire [7:0]  ctx_len,
    output wire        done,
    output wire        valid,
    output wire [511:0] ctilde,        // 定宽 512 位，低 ctb 字节有效
    output wire [511:0] ctilde_p,
    output wire [511:0] tr_out,
    output wire [511:0] mu,
    output wire        zbad,
    output wire        hbad,
    input  wire [6:0]  dbg_sel,
    input  wire [7:0]  dbg_idx,
    output wire signed [31:0] dbg_coef
);
    wire        ss, siv, sif, sor;
    wire [7:0]  sr, su, sid, sod;
    wire        sir, sov;

    // 核已改成运行时选参数集；由 K 反推 pset，让既有九格脚本不用改。
    localparam [1:0] PSET_FROM_K = (K == 4) ? 2'd0 : (K == 6) ? 2'd1 : 2'd2;

    mldsa_verify u_ver (
        .clk(clk), .rst_n(rst_n), .pset(PSET_FROM_K), .start(start),
        .pk_wr_en(pk_wr_en), .pk_wr_addr(pk_wr_addr), .pk_wr_data(pk_wr_data),
        .sig_wr_en(sig_wr_en), .sig_wr_addr(sig_wr_addr), .sig_wr_data(sig_wr_data),
        .msg_wr_en(msg_wr_en), .msg_wr_addr(msg_wr_addr), .msg_wr_data(msg_wr_data),
        .ctx_wr_en(ctx_wr_en), .ctx_wr_addr(ctx_wr_addr), .ctx_wr_data(ctx_wr_data),
        .msg_len(msg_len), .ctx_len(ctx_len),
        .done(done), .valid(valid),
        .sha_start(ss), .sha_rate(sr), .sha_suffix(su),
        .sha_in_valid(siv), .sha_in_data(sid), .sha_in_flush(sif),
        .sha_in_ready(sir), .sha_out_valid(sov), .sha_out_ready(sor),
        .sha_out_data(sod),
        .ctilde(ctilde), .ctilde_p(ctilde_p), .tr_out(tr_out), .mu(mu),
        .zbad(zbad), .hbad(hbad),
        .dbg_sel(dbg_sel), .dbg_idx(dbg_idx), .dbg_coef(dbg_coef));

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
