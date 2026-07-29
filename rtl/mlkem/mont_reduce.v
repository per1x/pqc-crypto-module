// mont_reduce —— Montgomery 约减（FIPS 203, q = 3329）
//
// out ≡ a · 2^-16 (mod q)，输出范围 (-q, q)，要求 |a| < q·2^15。
//
// 与 C 参考实现逐位等价的关键点：
//   int16_t m = (int16_t)a * QINV;      ← **截断到 16 位有符号**
//   return (a - (int32_t)m * Q) >> 16;  ← 算术右移
// 截断这一步是本模块唯一容易写错的地方：m 必须只保留低 16 位再当有符号数用，
// 否则 (a - m*Q) 不会被 2^16 整除，右移出来的结果就是错的。
//
// 纯组合逻辑，不依赖任何厂商 IP —— 这样 cocotb + Verilator/Icarus 就能跑，
// 将来也能直接移植到 Lattice/国产器件（路线图 §5.3.1 分层的用意）。
`default_nettype none

module mont_reduce (
    input  wire signed [31:0] a,
    output wire signed [15:0] t_out
);
    localparam signed [15:0] Q    =  16'sd3329;
    localparam signed [15:0] QINV = -16'sd3327;   // q^-1 mod 2^16（= 62209 的有符号写法）

    // 低 16 位相乘后截断回 16 位有符号 —— 对应 C 里的 (int16_t) 赋值
    wire signed [15:0] m = $signed(a[15:0]) * QINV;

    wire signed [31:0] prod = m * Q;
    assign t_out = (a - prod) >>> 16;             // 算术右移
endmodule

`default_nettype wire
