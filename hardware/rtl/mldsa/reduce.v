// mldsa_reduce32 / mldsa_caddq —— ML-DSA 的两个常用归约（FIPS 204, q = 8380417）
//
// reduce32：把 32 位有符号系数折进大致 (−6283009, 6283008]。
//   t = (a + 2²²) >>> 23;  r = a − t·q
// 中间量 t·q 的最大绝对值是 256·q = 2145386752 < 2³¹，所以 32 位运算不会溢出，
// 与 C 参考实现用 int32_t 的行为一致。
//
// caddq：负系数加一个 q，把结果折回 [0, q)。参考实现写成
//   a += (a >> 31) & Q
// 是为了避免分支；硬件里选择器本身就与数据无关，所以直接写成条件形式。
`default_nettype none

module mldsa_reduce32 (
    input  wire signed [31:0] a,
    output wire signed [31:0] r
);
    localparam signed [31:0] Q = 32'sd8380417;

    wire signed [31:0] t = (a + 32'sd4194304) >>> 23;   // +2²² 后算术右移 23
    assign r = a - t * Q;
endmodule

module mldsa_caddq (
    input  wire signed [31:0] a,
    output wire signed [31:0] r
);
    localparam signed [31:0] Q = 32'sd8380417;

    assign r = a[31] ? (a + Q) : a;
endmodule

`default_nettype wire
