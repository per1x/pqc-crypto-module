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
`default_nettype none

module mlkem_basemul (
    input  wire signed [15:0] a0,
    input  wire signed [15:0] a1,
    input  wire signed [15:0] b0,
    input  wire signed [15:0] b1,
    input  wire signed [15:0] zeta,
    output wire signed [15:0] r0,
    output wire signed [15:0] r1
);

    // 位宽全部显式：Verilator 是 2-state，隐式截断会真的算错
    function automatic signed [31:0] smul;
        input signed [15:0] x;
        input signed [15:0] y;
        begin
            smul = $signed({{16{x[15]}}, x}) * $signed({{16{y[15]}}, y});
        end
    endfunction

    // a1·b1 → 约减 → 乘 ζ → 再约减
    wire signed [15:0] t_a1b1;
    mont_reduce u_a1b1 (.a(smul(a1, b1)), .t_out(t_a1b1));

    wire signed [15:0] t_zeta;
    mont_reduce u_zeta (.a(smul(t_a1b1, zeta)), .t_out(t_zeta));

    wire signed [15:0] t_a0b0;
    mont_reduce u_a0b0 (.a(smul(a0, b0)), .t_out(t_a0b0));

    wire signed [15:0] t_a0b1;
    mont_reduce u_a0b1 (.a(smul(a0, b1)), .t_out(t_a0b1));

    wire signed [15:0] t_a1b0;
    mont_reduce u_a1b0 (.a(smul(a1, b0)), .t_out(t_a1b0));

    assign r0 = t_zeta + t_a0b0;
    assign r1 = t_a0b1 + t_a1b0;
endmodule

`default_nettype wire
