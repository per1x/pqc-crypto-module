// mldsa_params —— 参数集译码表：pset(0/1/2) → 三个核要用的全部常量
//
// ============================================================================
// 【为什么是一个模块，而不是每个核各写一份】
// ============================================================================
// 运行时选参数集之后，K/ℓ/η/τ/γ₁/γ₂/ω/β/c̃ 以及由它们派生的十几个量
// （打包位宽、每条字节数、sk 段偏移、pk/sk/σ 长度、范数门限…）都要从常量变成信号。
// 这张表如果在 keygen/sign/verify 里各抄一份，就有**三处可能抄错**，
// 而抄错的表现是"某一个参数集下某一段偏移差几个字节"——九格里只有那一格会红，
// 另外八格全绿，最容易被当成偶发。
//
// 所以集中成一处：FIPS 204 表 1/表 2 在本仓库**只出现在这个文件里**。
//
// ⚠️ 全部写成显式常量而不是现算（例如 sk_s2 直接给 512/768/800，
//    而不是写 128 + l*peb）：现算要在硬件里做乘法，而这些量在每个参数集下
//    都是定值。表里的每一行都由 scratch 里的脚本按定义算出来、
//    并与 FIPS 204 表 2 的 pk/sk/σ 长度对过账（三组全中）。
//
// 本模块是**纯组合译码**，没有时序。调用方在 start 那一拍锁存 pset，
// 之后整个运算过程中喂给本模块的都是锁存后的值。
`default_nettype none

module mldsa_params (
    input  wire [1:0] pset,          // 0=ML-DSA-44  1=ML-DSA-65  2=ML-DSA-87

    // ---- 表 1 的直接项 ----
    output wire [3:0]  k,            // 4 / 6 / 8
    output wire [3:0]  l,            // 4 / 5 / 7
    output wire [2:0]  eta,          // 2 / 4 / 2
    output wire [6:0]  tau,          // 39 / 49 / 60
    output wire [7:0]  omega,        // 80 / 55 / 75
    output wire [6:0]  ctb,          // c̃ 字节数 32 / 48 / 64
    output wire        g2mode,       // 0 = γ₂=(q−1)/88，1 = (q−1)/32

    // ---- 循环边界（省得每处都写 −1）----
    output wire [3:0]  km1,
    output wire [3:0]  lm1,
    output wire [3:0]  lkm1,         // ℓ+k−1

    // ---- 打包位宽与每条多项式的字节数 ----
    output wire [4:0]  ebits,        // s₁/s₂ 每系数 3 / 4
    output wire [7:0]  peb,          // 每条 s 打包字节 96 / 128
    output wire [4:0]  zbits,        // z 每系数 18 / 20
    output wire [9:0]  zb,           // 每条 z 字节 576 / 640
    output wire [4:0]  w1bits,       // w₁ 每系数 6 / 4
    output wire [7:0]  w1b,          // 每条 w₁ 字节 192 / 128

    // ---- 32 位有符号常量 ----
    output wire signed [31:0] gamma1,
    output wire signed [31:0] gamma2,
    output wire signed [31:0] eta_s,

    // ---- 范数门限 ----
    output wire [31:0] zbound,       // γ₁ − β
    output wire [31:0] r0bound,      // γ₂ − β
    output wire [31:0] ct0bound,     // γ₂

    // ---- sk 段偏移与三个长度 ----
    output wire [12:0] sk_s2,        // s₂pack 起点（s₁pack 恒从 128 起）
    output wire [12:0] sk_t0,        // t₀pack 起点
    output wire [12:0] sklen,        // 2560 / 4032 / 4896
    output wire [13:0] pklen,        // 1312 / 1952 / 2592
    output wire [12:0] siglen,       // 2420 / 3309 / 4627
    output wire [12:0] sig_h0        // hint 段起点 = c̃ + ℓ·zb
);
    localparam [1:0] P44 = 2'd0, P65 = 2'd1;

    wire is44 = (pset == P44);
    wire is65 = (pset == P65);

    assign k      = is44 ? 4'd4  : is65 ? 4'd6  : 4'd8;
    assign l      = is44 ? 4'd4  : is65 ? 4'd5  : 4'd7;
    assign eta    = is44 ? 3'd2  : is65 ? 3'd4  : 3'd2;
    assign tau    = is44 ? 7'd39 : is65 ? 7'd49 : 7'd60;
    assign omega  = is44 ? 8'd80 : is65 ? 8'd55 : 8'd75;
    assign ctb    = is44 ? 7'd32 : is65 ? 7'd48 : 7'd64;
    assign g2mode = is44 ? 1'b0  : 1'b1;

    assign km1  = k - 4'd1;
    assign lm1  = l - 4'd1;
    assign lkm1 = l + k - 4'd1;

    assign ebits  = is44 ? 5'd3   : is65 ? 5'd4   : 5'd3;
    assign peb    = is44 ? 8'd96  : is65 ? 8'd128 : 8'd96;
    assign zbits  = is44 ? 5'd18  : 5'd20;
    assign zb     = is44 ? 10'd576 : 10'd640;
    assign w1bits = is44 ? 5'd6   : 5'd4;
    assign w1b    = is44 ? 8'd192 : 8'd128;

    assign gamma1 = is44 ? 32'sd131072 : 32'sd524288;   // 2¹⁷ / 2¹⁹
    assign gamma2 = is44 ? 32'sd95232  : 32'sd261888;   // (q−1)/88 / (q−1)/32
    assign eta_s  = {29'd0, eta};

    // β = 78 / 196 / 120
    wire [31:0] beta32 = is44 ? 32'd78 : is65 ? 32'd196 : 32'd120;
    assign zbound   = {gamma1} - beta32;
    assign r0bound  = {gamma2} - beta32;
    assign ct0bound = {gamma2};

    assign sk_s2  = is44 ? 13'd512  : is65 ? 13'd768  : 13'd800;
    assign sk_t0  = is44 ? 13'd896  : is65 ? 13'd1536 : 13'd1568;
    assign sklen  = is44 ? 13'd2560 : is65 ? 13'd4032 : 13'd4896;
    assign pklen  = is44 ? 14'd1312 : is65 ? 14'd1952 : 14'd2592;
    assign siglen = is44 ? 13'd2420 : is65 ? 13'd3309 : 13'd4627;
    assign sig_h0 = is44 ? 13'd2336 : is65 ? 13'd3248 : 13'd4544;
endmodule

`default_nettype wire
