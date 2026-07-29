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
// 这两个模块是各自数学的唯一实现，mldsa_ntt_core 直接例化它们，不再内联重写。
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

`default_nettype wire
