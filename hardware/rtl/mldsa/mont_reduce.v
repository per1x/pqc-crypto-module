// mldsa_mont_reduce —— Montgomery 约减（FIPS 204, q = 8380417）
//
// out ≡ a · 2⁻³² (mod q)，输出范围 (−q, q)，要求 |a| < q·2³¹。
//
// 与 ML-KEM 侧的 mont_reduce 是**两套不同的算术**：模数不同（8380417 对 3329），
// Montgomery 基不同（2³² 对 2¹⁶），系数位宽不同（32 位对 16 位）。
// 两者不能互相替代，所以 RTL 里分成 mldsa_ 与无前缀的两组模块。
//
// 与 C 参考实现逐位等价的关键点：
//   int32_t m = (int32_t)a * QINV;      ← **截断到 32 位有符号**
//   return (a - (int64_t)m * Q) >> 32;  ← 算术右移
// 截断这一步是本模块唯一容易写错的地方：m 必须只保留低 32 位再当有符号数用，
// 否则 (a − m·q) 不会被 2³² 整除。
//
// 每一步的位宽都显式写出：Verilator 是 2-state，隐式截断会真的算错，
// 而 Icarus 的 4-state 可能碰巧对上。两个仿真器都跑就是为了逼出这类差异。
`default_nettype none

module mldsa_mont_reduce (
    input  wire signed [63:0] a,
    output wire signed [31:0] t_out
);
    localparam signed [31:0] Q    = 32'sd8380417;
    localparam signed [31:0] QINV = 32'sd58728449;   // q⁻¹ mod 2³²

    // 低 32 位相乘后截断回 32 位有符号 —— 对应 C 里的 (int32_t) 赋值
    wire signed [31:0] m = $signed(a[31:0]) * QINV;

    wire signed [63:0] prod = $signed({{32{m[31]}}, m}) * $signed({{32{Q[31]}}, Q});
    wire signed [63:0] diff = a - prod;
    assign t_out = diff[63:32];                      // 算术右移 32 == 取高 32 位
endmodule

`default_nettype wire
