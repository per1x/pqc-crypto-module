// mlkem_basemul —— NTT 域的基乘（FIPS 203, q = 3329）
//
// ML-KEM 的 NTT 只做 7 层，变换结果不是 256 个标量，而是 128 个一次多项式，
// 每一对系数落在 Z_q[x]/(x² − ζ) 里。所以 NTT 域的"逐点乘"实际是：
//
//     (a0 + a1·x)(b0 + b1·x) mod (x² − ζ)
//       = (a0·b0 + a1·b1·ζ) + (a0·b1 + a1·b0)·x
//
// 所有乘法都在 Montgomery 域完成，与 mont_reduce.v 的约定一致：
// fqmul(a, b) = mont(a·b) ≡ a·b·2⁻¹⁶ (mod q)。因此
//
//     r0 = fqmul(fqmul(a1, b1), ζ) + fqmul(a0, b0)
//     r1 = fqmul(a0, b1)           + fqmul(a1, b0)
//
// a1·b1 先约减再乘 ζ，是为了把中间结果压回 (−q, q)，
// 保证第二次 mont_reduce 的输入仍满足 |a| < q·2¹⁵。
//
// 一对系数用 +ζ，相邻一对用 −ζ（x² − ζ 与 x² + ζ 交替），
// 符号由调用方在 zeta 端口上给出，本模块不做区分。
//
// 纯组合逻辑，五个乘法器 —— 真实的核里这五次乘法会分时复用同一块 DSP。
//
// 【为什么拆成 _head / _tail 两半】
// r0 这条路上串着 6 次乘法：smul(a1,b1) 一次、它的 mont_reduce 两次
// （m = a·QINV、m·Q）、smul(·,ζ) 一次、再一次 mont_reduce 两次。
// 在 ZU3EG 上一级 DSP 乘加大约 2.4 ns，6 级就是 14 ns 上下 —— 再接上调用方
// 的 barrett（又是 2 次乘法）直接超过 20 ns，**100 MHz 收不住**。
// 实测 mlkem_keygen 第一版 WNS −10.722 ns，关键路径正是这一条。
//
// 所以把它按 t_a1b1 切成两半，让**需要跑 100 MHz 的核**（mlkem_keygen）
// 在中间插一级寄存器；组合版 mlkem_basemul 仍然由这两半拼出来，
// 接口和原有的 cocotb 用例都不变 —— 同一段数学只有一份实现。
`default_nettype none

// 前半：a1·b1 的那一次 fqmul。切在这里是因为它正好把 6 级乘法分成 3 + 3。
module mlkem_basemul_head (
    input  wire signed [15:0] a1,
    input  wire signed [15:0] b1,
    output wire signed [15:0] t_a1b1
);
    mont_reduce u_a1b1 (
        .a($signed({{16{a1[15]}}, a1}) * $signed({{16{b1[15]}}, b1})),
        .t_out(t_a1b1));
endmodule

// 后半：剩下的四次 fqmul 与两个加法。四次之间互相独立，深度仍是 3 级乘法。
module mlkem_basemul_tail (
    input  wire signed [15:0] a0,
    input  wire signed [15:0] a1,
    input  wire signed [15:0] b0,
    input  wire signed [15:0] b1,
    input  wire signed [15:0] zeta,
    input  wire signed [15:0] t_a1b1,
    output wire signed [15:0] r0,
    output wire signed [15:0] r1
);
    function automatic signed [31:0] smul;
        input signed [15:0] x;
        input signed [15:0] y;
        begin
            smul = $signed({{16{x[15]}}, x}) * $signed({{16{y[15]}}, y});
        end
    endfunction

    wire signed [15:0] t_zeta, t_a0b0, t_a0b1, t_a1b0;
    mont_reduce u_zeta (.a(smul(t_a1b1, zeta)), .t_out(t_zeta));
    mont_reduce u_a0b0 (.a(smul(a0, b0)),       .t_out(t_a0b0));
    mont_reduce u_a0b1 (.a(smul(a0, b1)),       .t_out(t_a0b1));
    mont_reduce u_a1b0 (.a(smul(a1, b0)),       .t_out(t_a1b0));

    assign r0 = t_zeta + t_a0b0;
    assign r1 = t_a0b1 + t_a1b0;
endmodule

module mlkem_basemul (
    input  wire signed [15:0] a0,
    input  wire signed [15:0] a1,
    input  wire signed [15:0] b0,
    input  wire signed [15:0] b1,
    input  wire signed [15:0] zeta,
    output wire signed [15:0] r0,
    output wire signed [15:0] r1
);

    // 组合版 = 前半 + 后半直接串起来，不插寄存器。
    // 保留它是因为它是"基乘"这件事最直白的写法，也是 cocotb 组合用例的被测对象。
    wire signed [15:0] t_a1b1;
    mlkem_basemul_head u_head (.a1(a1), .b1(b1), .t_a1b1(t_a1b1));
    mlkem_basemul_tail u_tail (
        .a0(a0), .a1(a1), .b0(b0), .b1(b1), .zeta(zeta),
        .t_a1b1(t_a1b1), .r0(r0), .r1(r1));
endmodule

`default_nettype wire
