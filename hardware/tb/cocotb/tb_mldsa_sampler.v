// 把两个 ML-DSA 采样器各自接上一个 sha3_core，供 cocotb 当顶层用
//
// 采样器本身不含海绵 —— KeyGen 全程共用一个 sha3_core（面积上唯一合理的
// 做法），所以采样器把 SHAKE 的接口整个引出来。测试台在这里替它补上。
//
// 两个采样器各配一个海绵，是为了让两条用例互不影响：共用一个的话，
// 一条用例把海绵停在半途，另一条就会拿到错的字节流，
// 而失败原因会指向被测的那个采样器 —— 完全指错方向。
`default_nettype none

module tb_mldsa_sampler (
    input  wire clk,
    input  wire rst_n,

    // ---- uniform（SHAKE128）----
    input  wire         u_start,
    input  wire [255:0] u_seed,
    input  wire [15:0]  u_nonce,
    output wire         u_done,
    input  wire [7:0]   u_rd_addr,
    output wire [22:0]  u_rd_data,
    output wire [8:0]   u_count,

    // ---- eta（SHAKE256）----
    input  wire         e_start,
    input  wire [511:0] e_seed,
    input  wire [15:0]  e_nonce,
    output wire         e_done,
    input  wire [7:0]   e_rd_addr,
    output wire signed [31:0] e_rd_data,
    output wire [8:0]   e_count
);
    // ---------------- uniform ----------------
    wire        us_start, us_iv, us_if, us_ir, us_ov, us_or;
    wire [7:0]  us_rate, us_suf, us_id, us_od;

    mldsa_poly_uniform u_uni (
        .clk(clk), .rst_n(rst_n),
        .start(u_start), .seed(u_seed), .nonce(u_nonce), .done(u_done),
        .sha_start(us_start), .sha_rate(us_rate), .sha_suffix(us_suf),
        .sha_in_valid(us_iv), .sha_in_data(us_id), .sha_in_flush(us_if),
        .sha_in_ready(us_ir), .sha_out_valid(us_ov), .sha_out_ready(us_or),
        .sha_out_data(us_od),
        .rd_addr(u_rd_addr), .rd_data(u_rd_data), .count(u_count));

    sha3_core u_sha_u (
        .clk(clk), .rst_n(rst_n),
        .rate_bytes(us_rate), .suffix(us_suf),
        .start(us_start), .zeroize(1'b0),
        .in_valid(us_iv), .in_ready(us_ir), .in_data(us_id), .in_flush(us_if),
        .out_valid(us_ov), .out_ready(us_or), .out_data(us_od),
        .busy(), .absorbing(), .squeezing(),
        .ext_start(1'b0), .ext_done(), .ext_wr_en(1'b0),
        .ext_wr_addr(5'd0), .ext_wr_data(64'd0), .ext_rd_addr(5'd0),
        .ext_rd_data());

    // ---------------- eta ----------------
    wire        es_start, es_iv, es_if, es_ir, es_ov, es_or;
    wire [7:0]  es_rate, es_suf, es_id, es_od;

    mldsa_poly_eta #(.ETA(2)) u_eta (
        .clk(clk), .rst_n(rst_n),
        .start(e_start), .seed(e_seed), .nonce(e_nonce), .done(e_done),
        .sha_start(es_start), .sha_rate(es_rate), .sha_suffix(es_suf),
        .sha_in_valid(es_iv), .sha_in_data(es_id), .sha_in_flush(es_if),
        .sha_in_ready(es_ir), .sha_out_valid(es_ov), .sha_out_ready(es_or),
        .sha_out_data(es_od),
        .rd_addr(e_rd_addr), .rd_data(e_rd_data), .count(e_count));

    sha3_core u_sha_e (
        .clk(clk), .rst_n(rst_n),
        .rate_bytes(es_rate), .suffix(es_suf),
        .start(es_start), .zeroize(1'b0),
        .in_valid(es_iv), .in_ready(es_ir), .in_data(es_id), .in_flush(es_if),
        .out_valid(es_ov), .out_ready(es_or), .out_data(es_od),
        .busy(), .absorbing(), .squeezing(),
        .ext_start(1'b0), .ext_done(), .ext_wr_en(1'b0),
        .ext_wr_addr(5'd0), .ext_wr_data(64'd0), .ext_rd_addr(5'd0),
        .ext_rd_data());
endmodule
