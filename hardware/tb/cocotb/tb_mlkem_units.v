// tb_mlkem_units —— 仅供仿真的汇总顶层
//
// cocotb 一次运行只驱动一个顶层。把 ML-KEM 数据通路里那几个小模块
// （CBD 采样、拒绝采样、12 位编解码）挂在同一个顶层下，一次仿真即可全部覆盖，
// 而不必为每个组合逻辑模块各起一次仿真。
//
// 本文件不参与综合，也不在 hardware/rtl/ 下 —— 它只是端口的转接。
`default_nettype none

module tb_mlkem_units (
    input  wire         clk,
    input  wire         rst_n,

    // 中心二项分布采样
    input  wire [31:0]  cbd2_in,
    output wire [127:0] cbd2_out,
    input  wire [23:0]  cbd3_in,
    output wire [63:0]  cbd3_out,

    // 拒绝采样：取候选
    input  wire [23:0]  rej_in,
    output wire [11:0]  rej_d1,
    output wire [11:0]  rej_d2,
    output wire         rej_ok1,
    output wire         rej_ok2,

    // 拒绝采样：收集器
    input  wire         rej_start,
    output wire         rej_done,
    input  wire         rej_valid,
    input  wire [23:0]  rej_bytes,
    output wire         rej_ready,
    output wire [8:0]   rej_count,
    input  wire [7:0]   rej_addr,
    output wire [15:0]  rej_data,

    // 12 位编解码
    input  wire signed [15:0] enc_c0,
    input  wire signed [15:0] enc_c1,
    output wire [23:0]  enc_bytes,
    input  wire [23:0]  dec_bytes,
    output wire [11:0]  dec_c0,
    output wire [11:0]  dec_c1,

    // CBD 流式采样器
    input  wire         cbs_eta3,
    input  wire         cbs_start,
    output wire         cbs_done,
    input  wire         cbs_in_valid,
    output wire         cbs_in_ready,
    input  wire [7:0]   cbs_in_data,
    output wire         cbs_out_valid,
    input  wire         cbs_out_ready,
    output wire signed [15:0] cbs_out_coeff,
    output wire [8:0]   cbs_count,

    // 变宽度 ByteEncode_d / ByteDecode_d
    input  wire [3:0]   bp_d,
    input  wire         bp_in_valid,
    output wire         bp_in_ready,
    input  wire [11:0]  bp_in_data,
    output wire         bp_out_valid,
    input  wire         bp_out_ready,
    output wire [7:0]   bp_out_data,

    input  wire [3:0]   bu_d,
    input  wire         bu_in_valid,
    output wire         bu_in_ready,
    input  wire [7:0]   bu_in_data,
    output wire         bu_out_valid,
    input  wire         bu_out_ready,
    output wire [11:0]  bu_out_data
);

    mlkem_cbd2 u_cbd2 (.rand_in(cbd2_in), .coeffs(cbd2_out));
    mlkem_cbd3 u_cbd3 (.rand_in(cbd3_in), .coeffs(cbd3_out));

    mlkem_rej_pair u_rej_pair (
        .bytes_in(rej_in), .d1(rej_d1), .d2(rej_d2),
        .d1_ok(rej_ok1), .d2_ok(rej_ok2));

    mlkem_rej_uniform u_rej_uniform (
        .clk(clk), .rst_n(rst_n),
        .start(rej_start), .done(rej_done),
        .in_valid(rej_valid), .in_bytes(rej_bytes), .in_ready(rej_ready),
        .count(rej_count), .rd_addr(rej_addr), .rd_data(rej_data));

    mlkem_cbd_stream u_cbd_stream (
        .clk(clk), .rst_n(rst_n),
        .eta3(cbs_eta3), .start(cbs_start), .done(cbs_done),
        .in_valid(cbs_in_valid), .in_ready(cbs_in_ready), .in_data(cbs_in_data),
        .out_valid(cbs_out_valid), .out_ready(cbs_out_ready),
        .out_coeff(cbs_out_coeff), .count(cbs_count));

    mlkem_bitpack u_bitpack (
        .clk(clk), .rst_n(rst_n), .d(bp_d),
        .in_valid(bp_in_valid), .in_ready(bp_in_ready), .in_data(bp_in_data),
        .out_valid(bp_out_valid), .out_ready(bp_out_ready), .out_data(bp_out_data));

    mlkem_bitunpack u_bitunpack (
        .clk(clk), .rst_n(rst_n), .d(bu_d),
        .in_valid(bu_in_valid), .in_ready(bu_in_ready), .in_data(bu_in_data),
        .out_valid(bu_out_valid), .out_ready(bu_out_ready), .out_data(bu_out_data));

    mlkem_encode12 u_encode12 (.c0(enc_c0), .c1(enc_c1), .bytes_out(enc_bytes));
    mlkem_decode12 u_decode12 (.bytes_in(dec_bytes), .c0(dec_c0), .c1(dec_c1));
endmodule

`default_nettype wire
