// tb_mldsa_units —— 仅供仿真的汇总顶层
//
// cocotb 一次运行只驱动一个顶层。把 ML-DSA 数据通路里的算子、蝶形、高低位拆分、
// 提示位与采样挂在同一个顶层下，一次仿真即可全部覆盖。
//
// 两个参数集（γ₂ = (q−1)/88 与 (q−1)/32）各例化一份，测试台同时比对，
// 避免"只测了一个 MODE、另一个的常数写错没人发现"。
//
// 本文件不参与综合，也不在 hardware/rtl/ 下 —— 它只是端口的转接。
`default_nettype none

module tb_mldsa_units (
    input  wire               clk,
    input  wire               rst_n,

    // L0 算子
    input  wire signed [63:0] mont_a,
    output wire signed [31:0] mont_out,
    input  wire signed [31:0] red_a,
    output wire signed [31:0] red_out,
    input  wire signed [31:0] cadd_a,
    output wire signed [31:0] cadd_out,

    // 蝶形
    input  wire signed [31:0] bf_a,
    input  wire signed [31:0] bf_b,
    input  wire signed [31:0] bf_zeta,
    output wire signed [31:0] ct_ao,
    output wire signed [31:0] ct_bo,
    output wire signed [31:0] gs_ao,
    output wire signed [31:0] gs_bo,

    // 流水版蝶形（综合真正用的那个）—— 与上面的组合版对同一份向量
    input  wire               bfp_valid,
    input  wire               bfp_mode,
    input  wire               bfp_scale,
    output wire               bfp_out_valid,
    output wire signed [31:0] bfp_ao,
    output wire signed [31:0] bfp_bo,

    // 高低位拆分
    input  wire signed [31:0] rnd_a,
    output wire signed [31:0] p2r_a0,
    output wire        [9:0]  p2r_a1,
    output wire signed [31:0] d88_a0,
    output wire        [5:0]  d88_a1,
    output wire signed [31:0] d32_a0,
    output wire        [5:0]  d32_a1,

    // 提示位
    input  wire signed [31:0] mh_a0,
    input  wire        [5:0]  mh_a1,
    output wire               mh88,
    output wire               mh32,
    input  wire signed [31:0] uh_a,
    input  wire               uh_hint,
    output wire        [5:0]  uh88,
    output wire        [5:0]  uh32,

    // 采样
    input  wire [23:0]        ru_bytes,
    output wire [22:0]        ru_cand,
    output wire               ru_ok,
    input  wire [3:0]         re_nib,
    output wire signed [31:0] re2_coeff,
    output wire               re2_ok,
    output wire signed [31:0] re4_coeff,
    output wire               re4_ok,

    // 均匀采样收集器
    input  wire               rb_start,
    output wire               rb_done,
    input  wire               rb_valid,
    input  wire [23:0]        rb_bytes,
    output wire               rb_ready,
    output wire [8:0]         rb_count,
    input  wire [7:0]         rb_addr,
    output wire [22:0]        rb_data
);

    mldsa_mont_reduce u_mont (.a(mont_a), .t_out(mont_out));
    mldsa_reduce32    u_red  (.a(red_a),  .r(red_out));
    mldsa_caddq       u_cadd (.a(cadd_a), .r(cadd_out));

    mldsa_butterfly_ct u_ct (
        .a(bf_a), .b(bf_b), .zeta(bf_zeta), .a_out(ct_ao), .b_out(ct_bo));
    mldsa_butterfly_gs u_gs (
        .a(bf_a), .b(bf_b), .zeta(bf_zeta), .a_out(gs_ao), .b_out(gs_bo));

    // 流水版：同样的 a/b/ζ 输入，mode 选 CT/GS，5 拍后出结果。
    // 组合版是"数学的参照"，这一份是**真正综合进 bitstream 的**，
    // 两者对同一份 mldsa_butterfly.hex 都必须逐位一致。
    mldsa_butterfly_pipe #(.TAGW(8)) u_bfp (
        .clk(clk), .rst_n(rst_n),
        .in_valid(bfp_valid), .in_tag(8'd0),
        .mode(bfp_mode), .scale(bfp_scale),
        .a(bf_a), .b(bf_b), .zeta(bf_zeta),
        .out_valid(bfp_out_valid), .out_tag(), .out_scale(),
        .a_out(bfp_ao), .b_out(bfp_bo), .pipe_busy());

    mldsa_power2round u_p2r (.a(rnd_a), .a0(p2r_a0), .a1(p2r_a1));
    // MODE 已是运行时口：两份例化只是把 mode 各接死成 0/1，
    // 好让同一条用例仍能一次比完两种 γ₂（这本身也证明了同一份 RTL 两种都对）
    mldsa_decompose u_d88 (.mode(1'b0), .a(rnd_a), .a0(d88_a0), .a1(d88_a1));
    mldsa_decompose u_d32 (.mode(1'b1), .a(rnd_a), .a0(d32_a0), .a1(d32_a1));

    mldsa_make_hint u_mh88 (.mode(1'b0), .a0(mh_a0), .a1(mh_a1), .hint(mh88));
    mldsa_make_hint u_mh32 (.mode(1'b1), .a0(mh_a0), .a1(mh_a1), .hint(mh32));
    mldsa_use_hint  u_uh88 (.mode(1'b0), .a(uh_a), .hint(uh_hint), .a1_out(uh88));
    mldsa_use_hint  u_uh32 (.mode(1'b1), .a(uh_a), .hint(uh_hint), .a1_out(uh32));

    mldsa_rej_uniform u_ru (
        .bytes_in(ru_bytes), .cand(ru_cand), .cand_ok(ru_ok));
    // η 已是运行时口：两份例化只是把 eta 各接死成 2/4，
    // 好让同一条用例仍能一次比完两种 η（这本身也证明了同一份 RTL 两种都对）
    mldsa_rej_eta u_re2 (.eta(3'd2),
        .nibble(re_nib), .coeff(re2_coeff), .coeff_ok(re2_ok));
    mldsa_rej_eta u_re4 (.eta(3'd4),
        .nibble(re_nib), .coeff(re4_coeff), .coeff_ok(re4_ok));

    mldsa_rej_uniform_buf u_rb (
        .clk(clk), .rst_n(rst_n),
        .start(rb_start), .done(rb_done),
        .in_valid(rb_valid), .in_bytes(rb_bytes), .in_ready(rb_ready),
        .count(rb_count), .rd_addr(rb_addr), .rd_data(rb_data));
endmodule

`default_nettype wire
