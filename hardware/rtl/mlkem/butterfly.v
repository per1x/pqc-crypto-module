// butterfly —— NTT 蝶形单元（CT 与 GS 两种）+ Barrett 约减
//
// CT（Cooley-Tukey，正向 NTT）：
//     t = mont(zeta · b);  a' = a + t;  b' = a - t
// GS（Gentleman-Sande，逆 NTT）：
//     a' = barrett(a + b); b' = mont(zeta · (b - a))
//
// 两者都例化同一个 mont_reduce —— 真实的核里这也是同一块乘法器分时复用。
//
// **这三个模块是各自数学的唯一实现**：ntt_core 直接例化它们，核里不再内联一份。
// 同一段数学有两处实现时，改一处忘另一处就会漂移；即使两份都对，
// "cocotb 测的是不是核里真正跑的那份"也说不清。
//
// 位宽一律显式（理由同 mont_reduce.v）。
`default_nettype none

module barrett_reduce (
    input  wire signed [15:0] a,
    output wire signed [15:0] r
);
    localparam signed [15:0] Q = 16'sd3329;
    localparam signed [15:0] V = 16'sd20159;      // ((1<<26) + q/2) / q

    wire signed [31:0] prod = $signed({{16{V[15]}}, V}) * $signed({{16{a[15]}}, a});
    wire signed [31:0] shifted = (prod + 32'sd33554432) >>> 26;   // +(1<<25) 后 >>26
    wire signed [15:0] t = shifted[15:0];
    assign r = a - t * Q;
endmodule

module butterfly_ct (
    input  wire signed [15:0] a,
    input  wire signed [15:0] b,
    input  wire signed [15:0] zeta,
    output wire signed [15:0] a_out,
    output wire signed [15:0] b_out
);
    wire signed [31:0] prod =
        $signed({{16{zeta[15]}}, zeta}) * $signed({{16{b[15]}}, b});
    wire signed [15:0] t;
    mont_reduce u_mont (.a(prod), .t_out(t));
    assign a_out = a + t;
    assign b_out = a - t;
endmodule

module butterfly_gs (
    input  wire signed [15:0] a,
    input  wire signed [15:0] b,
    input  wire signed [15:0] zeta,
    output wire signed [15:0] a_out,
    output wire signed [15:0] b_out
);
    wire signed [15:0] sum = a + b;
    barrett_reduce u_barrett (.a(sum), .r(a_out));

    wire signed [15:0] diff = b - a;
    wire signed [31:0] prod =
        $signed({{16{zeta[15]}}, zeta}) * $signed({{16{diff[15]}}, diff});
    mont_reduce u_mont (.a(prod), .t_out(b_out));
endmodule

`default_nettype wire
