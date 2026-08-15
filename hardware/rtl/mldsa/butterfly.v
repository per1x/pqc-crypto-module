// mldsa_butterfly_ct / mldsa_butterfly_gs —— ML-DSA 的 NTT 蝶形单元
//
// CT（Cooley-Tukey，正向 NTT）：
//     t = mont(ζ · b);  a' = a + t;  b' = a − t
// GS（Gentleman-Sande，逆 NTT）：
//     a' = a + b;  b' = mont(ζ · (a − b))
//
// 逆变换里调用方送进来的是 −zetas[k]，符号由调用方给出，本模块不做区分。
//
// 两者都例化同一个 mldsa_mont_reduce —— 真实的核里这也是同一块乘法器分时复用。
//
// ⚠️ 这两个组合模块现在是**数学的参照实现**，不再进 mldsa_ntt_core。
// 综合用的是本文件末尾的 mldsa_butterfly_pipe（同一套数学，切成 5 级流水）。
// 两者由 test_mldsa_units.py 的 test_butterfly / test_butterfly_pipe
// **对同一份 mldsa_butterfly.hex 向量**各测一遍，逐位一致才算数 ——
// 这就是"一套数学两个实现"唯一可接受的形式：有对拍把它们钉在一起。
`default_nettype none

module mldsa_butterfly_ct (
    input  wire signed [31:0] a,
    input  wire signed [31:0] b,
    input  wire signed [31:0] zeta,
    output wire signed [31:0] a_out,
    output wire signed [31:0] b_out
);
    wire signed [63:0] prod =
        $signed({{32{zeta[31]}}, zeta}) * $signed({{32{b[31]}}, b});
    wire signed [31:0] t;
    mldsa_mont_reduce u_mont (.a(prod), .t_out(t));

    assign a_out = a + t;
    assign b_out = a - t;
endmodule

module mldsa_butterfly_gs (
    input  wire signed [31:0] a,
    input  wire signed [31:0] b,
    input  wire signed [31:0] zeta,
    output wire signed [31:0] a_out,
    output wire signed [31:0] b_out
);
    assign a_out = a + b;

    wire signed [31:0] diff = a - b;
    wire signed [63:0] prod =
        $signed({{32{zeta[31]}}, zeta}) * $signed({{32{diff[31]}}, diff});
    mldsa_mont_reduce u_mont (.a(prod), .t_out(b_out));
endmodule


// ============================================================================
// mldsa_butterfly_pipe —— 上面那套数学的 5 级流水版本，CT / GS / 缩放三合一
// ============================================================================
//
// 为什么要有它：组合版把**三次 32×32 乘法**串在一个时钟周期里
//   ζ·b  →  m = prod·QINV  →  m·Q
// 再加模约减与蝶形加减。ML-DSA-87 Sign 的 post-route 实测是
//   WNS @100MHz = −3.332ns（Fmax ≈ 75.0MHz），逻辑层级 37，
//   关键路径 u_ntt/k_reg → u_ntt/u_mem/DINADIN[29]，
// 系统时钟正好 75MHz，余量约 +0.001ns —— 进整体设计必挂。
//
// 切成 5 级之后每一级只剩一次乘法或一次加减，DSP48E2 的内部流水寄存器也能用上。
//
// 级次（in_valid 那一拍算第 0 拍，out_valid 在第 5 拍）：
//   1  选操作数：y = b / (a−b) / a，keep = a / (a+b)，x = ζ
//   2  p = x·y                     （32×32 → 64）
//   3  m = (int32)(p[31:0]·QINV)   （只要低 32 位）
//   4  mq = m·Q                    （32×32 → 64）
//   5  t = (p − mq) >>> 32         （64 位减 + 取高 32）
//   组合出口：a_out/b_out 的最后一次 32 位加减，直接进 BRAM 的 DIN
//
// 三种运算共用同一条乘法链（这也顺带把面积压下来：原来 ntt_core 里
// CT / GS / 缩放各例化一份 mont，等于三套乘法器）：
//   mode=0 scale=0  CT：t = mont(ζ·b);      a_out = a+t, b_out = a−t
//   mode=1 scale=0  GS：t = mont(ζ·(a−b));  a_out = a+b, b_out = t
//   mode=x scale=1  缩放：a_out = mont(ζ·a)（ζ 端由调用方送 f = mont²/256）
//
// **GS 的 ζ 由调用方取负**，与上面的组合版约定完全一致，好让两者能对同一份向量。
//
// in_tag 是一条跟着数据走的透明通道（NTT 核用它带写回地址）。让地址与数据在
// **同一个模块里**同步前进，就不可能出现"改了流水深度忘了改地址延迟"这类错位。
//
// ⚠️ 数据通路寄存器**一律不带复位**：DSP48E2 的内部流水寄存器只支持同步复位，
// 挂上异步复位 Vivado 就无法把它们塞进 DSP，流水白打、时序回到原点。
// 复位只加在 valid/tag/mode/scale 这条控制链上（它们本来就是 fabric 触发器）。
// 4-state 仿真下的 X 由 initial 块压掉，且 valid 没拉高时输出本就不该被采信。
module mldsa_butterfly_pipe #(
    parameter integer TAGW = 8
) (
    input  wire               clk,
    input  wire               rst_n,

    input  wire               in_valid,
    input  wire [TAGW-1:0]    in_tag,
    input  wire               mode,     // 0 = CT，1 = GS
    input  wire               scale,    // 1 = 只做 mont(ζ·a)
    input  wire signed [31:0] a,
    input  wire signed [31:0] b,
    input  wire signed [31:0] zeta,

    output wire               out_valid,
    output wire [TAGW-1:0]    out_tag,
    output wire               out_scale,
    output wire signed [31:0] a_out,
    output wire signed [31:0] b_out,

    // 流水线里还有没有在飞的运算。层与层之间要靠它排空。
    output wire               pipe_busy
);
    // 乘法链本身在 mldsa_mont_mul_pipe（mont_reduce.v）里 —— 全工程只此一份。
    // 本模块只剩三件事：选乘法器的操作数、把旁路值对齐、出口做最后一次加减。

    // 选乘法器的第二个操作数，并把旁路合成**一个** 32 位值：
    // CT 出口要 a（a+t / a−t），GS 出口要 a+b（且 b_out 只用 t），
    // 所以往下只需要传一个 keep，而不是 a 和 b 两条 —— 省一半旁路寄存器，
    // 顺带把 GS 的那次加法挪到进乘法器之前（那一级最闲）。
    wire signed [31:0] y_sel    = scale ? a : (mode ? (a - b) : b);
    wire signed [31:0] keep_sel = mode ? (a + b) : a;

    // ⚠️ keep / mode / scale 一律塞进 in_tag 跟着数据走，而不是另开一条等长的
    // 移位寄存器。寄存器数量完全一样，但"旁路比结果早/晚一拍"这类错位
    // **在结构上不可能发生** —— 乘法链改几级，旁路自动跟着改几级。
    wire [31:0] keep_o;
    wire        md_o, sc_o;
    wire signed [31:0] t_o;

    mldsa_mont_mul_pipe #(.TAGW(TAGW + 34)) u_mm (
        .clk(clk), .rst_n(rst_n),
        .in_valid(in_valid),
        .in_tag({in_tag, keep_sel, mode, scale}),
        .x(zeta), .y(y_sel),
        .out_valid(out_valid),
        .out_tag({out_tag, keep_o, md_o, sc_o}),
        .t_out(t_o),
        .pipe_busy(pipe_busy));

    // 出口：最后一次 32 位加减，组合送到 BRAM 的 DIN
    assign a_out = sc_o ? t_o : (md_o ? $signed(keep_o) : ($signed(keep_o) + t_o));
    assign b_out = md_o ? t_o : ($signed(keep_o) - t_o);
    assign out_scale = sc_o;
endmodule

`default_nettype wire
