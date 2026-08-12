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

// ============================================================================
// 【为什么拆成 _head / _tail 两半】
// ============================================================================
// 蝶形这条路是：BRAM 读出 → 一次乘法 → mont_reduce（内部又是两次乘法：
// m = a·QINV、m·Q）→ BRAM 写入。**三级 DSP 串在一起**，在 ZU3EG 上实测
// 9.7 ns，接不住 100 MHz —— mlkem_decaps 第一版 WNS −0.127 ns，关键路径
// 就是 u_enc/u_ntt/u_mem 的这条自环。
//
// 切在**乘积之后、Montgomery 归约之前**：前半 1 级乘法，后半 2 级。
// 切法与 basemul.v 的 _head/_tail 完全一样，理由也一样 ——
// 组合版 butterfly_ct / butterfly_gs 仍由这两半拼出来，接口与原有的
// cocotb 用例都不变，**同一段数学只有一份实现**。
//
// 这一刀值得在 ntt_core 里切、而不是让每个调用它的核各切各的：
// keygen / encaps / decaps 三个核的关键路径最后都收敛到这里。

module butterfly_ct_head (
    input  wire signed [15:0] b,
    input  wire signed [15:0] zeta,
    output wire signed [31:0] prod
);
    assign prod = $signed({{16{zeta[15]}}, zeta}) * $signed({{16{b[15]}}, b});
endmodule

module butterfly_ct_tail (
    input  wire signed [15:0] a,
    input  wire signed [31:0] prod,
    output wire signed [15:0] a_out,
    output wire signed [15:0] b_out
);
    wire signed [15:0] t;
    mont_reduce u_mont (.a(prod), .t_out(t));
    assign a_out = a + t;
    assign b_out = a - t;
endmodule

module butterfly_ct (
    input  wire signed [15:0] a,
    input  wire signed [15:0] b,
    input  wire signed [15:0] zeta,
    output wire signed [15:0] a_out,
    output wire signed [15:0] b_out
);
    wire signed [31:0] prod;
    butterfly_ct_head u_head (.b(b), .zeta(zeta), .prod(prod));
    butterfly_ct_tail u_tail (.a(a), .prod(prod), .a_out(a_out), .b_out(b_out));
endmodule

// GS 的前半多算一件事：a′ = barrett(a+b) 这条路是 2 级乘法，放前半正好和
// 后半的 mont（2 级）配平。放后半的话后半变成"2 级并 2 级"、前半只剩 1 级，
// 白白浪费一边。
module butterfly_gs_head (
    input  wire signed [15:0] a,
    input  wire signed [15:0] b,
    input  wire signed [15:0] zeta,
    output wire signed [31:0] prod,
    output wire signed [15:0] a_out
);
    wire signed [15:0] sum = a + b;
    barrett_reduce u_barrett (.a(sum), .r(a_out));

    wire signed [15:0] diff = b - a;
    assign prod = $signed({{16{zeta[15]}}, zeta}) * $signed({{16{diff[15]}}, diff});
endmodule

module butterfly_gs_tail (
    input  wire signed [31:0] prod,
    output wire signed [15:0] b_out
);
    mont_reduce u_mont (.a(prod), .t_out(b_out));
endmodule

module butterfly_gs (
    input  wire signed [15:0] a,
    input  wire signed [15:0] b,
    input  wire signed [15:0] zeta,
    output wire signed [15:0] a_out,
    output wire signed [15:0] b_out
);
    wire signed [31:0] prod;
    butterfly_gs_head u_head (
        .a(a), .b(b), .zeta(zeta), .prod(prod), .a_out(a_out));
    butterfly_gs_tail u_tail (.prod(prod), .b_out(b_out));
endmodule

`default_nettype wire
