// 把 mldsa_keygen 接上一个 sha3_core，供 cocotb 当顶层用（增量验证）
//
// 参数 K/L/ETA 透传给核，用 Icarus 的 -P 覆盖即可切参数集：
//   ML-DSA-44：K=4 L=4 ETA=2（默认）
//   ML-DSA-65：K=6 L=5 ETA=4
//   ML-DSA-87：K=8 L=7 ETA=2
`default_nettype none
module tb_mldsa_keygen #(
    parameter integer K   = 4,
    parameter integer L   = 4,
    parameter integer ETA = 2
) (
    input  wire clk, input  wire rst_n,
    input  wire        start,
    input  wire [255:0] xi,
    output wire        done,
    output wire [255:0] rho,
    output wire [511:0] rho_prime,
    output wire [255:0] key_out,
    input  wire [5:0]  dbg_sel,
    input  wire [7:0]  dbg_idx,
    output wire signed [31:0] dbg_coef,
    input  wire [12:0] pk_addr,
    output wire [7:0]  pk_data,
    input  wire [12:0] sk_addr,
    output wire [7:0]  sk_data
);
    wire        ss, siv, sif, sor;
    wire [7:0]  sr, su, sid, sod;
    wire        sir, sov;

    // 核已经改成**运行时选参数集**，不再有 K/L/ETA 参数。
    // 但既有的九格脚本（tools/mldsa_grid.sh）是用 -P K/L/ETA 选参数集的，
    // 所以这里由 K 反推 pset，让那批脚本一个字都不用改：
    //   K=4→44  K=6→65  K=8→87（K 在三个参数集里互不相同，反推是唯一的）
    // 运行时切换的用例走 tb_mldsa_engine（它的 pset 是真端口，可以中途改）。
    localparam [1:0] PSET_FROM_K = (K == 4) ? 2'd0 : (K == 6) ? 2'd1 : 2'd2;

    mldsa_keygen u_kg (
        .clk(clk), .rst_n(rst_n), .pset(PSET_FROM_K),
        .start(start), .xi(xi), .done(done),
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
`default_nettype wire
