// mldsa_power2round / mldsa_decompose / mldsa_make_hint / mldsa_use_hint
//                                        —— FIPS 204 的高低位拆分与提示位
//
// 【Power2Round】把系数拆成 a = a₁·2¹³ + a₀，a₀ 落在 (−2¹², 2¹²]。
// 公钥只发布 a₁，a₀ 留在私钥里。舍入常数是 2¹²−1 而不是 2¹²，
// 差一格就会让 a₀ 取不到区间端点。
//
// 【Decompose】把系数拆成 a = a₁·2γ₂ + a₀。除以 2γ₂ 用"乘倒数再右移"代替除法，
// 两个参数集各有一组常数：
//     γ₂ = (q−1)/32 → a₁ = ((a₁ₜ·1025 + 2²¹) >> 22) & 15
//     γ₂ = (q−1)/88 → a₁ = (a₁ₜ·11275 + 2²³) >> 24，再把 44 钳到 0
// 其中 a₁ₜ = (a + 127) >> 7。等价性由测试台在整个系数域上按代表元验证。
//
// 【提示位】签名把低位的进位信息压成 1 比特，验签据此还原高位。
// FIPS 204 依赖的性质是：对 |e| ≤ γ₂ 的扰动，
//     UseHint(r+e, MakeHint(r₀+e, r₁)) == r₁
// 测试台直接验这一条，而不是只验 MakeHint 的真值表。
//
// MODE 选参数集：0 = (q−1)/88（ML-DSA-44），1 = (q−1)/32（ML-DSA-65 / 87）。
// 所有模块的系数输入都要求已经落在 [0, q)。
`default_nettype none

module mldsa_power2round (
    input  wire signed [31:0] a,
    output wire signed [31:0] a0,
    output wire        [9:0]  a1
);
    // a < 2²³，(a + 2¹² − 1) >> 13 落在 [0, 1024)
    wire [23:0] t = (a[23:0] + 24'd4095) >> 13;
    assign a1 = t[9:0];
    assign a0 = a - $signed({9'd0, t[9:0], 13'd0});
endmodule

// ⚠️ γ₂ 的选择 mode 是**运行时输入**，不再是编译期参数。
//    理由与 pack.v 那边一样：同一个 bitstream 要能运行时选 44/65/87，
//    而 44 用 γ₂=(q−1)/88、65/87 用 (q−1)/32。
//    两支的乘法器常量与移位量都不同，所以两支**并行算完再选**，
//    而不是去 mux 乘法器的操作数 —— 后者省不下什么（17×17 很小），
//    却要把两套"乘数/偏置/取哪几位"的对应关系揉进一条式子，很容易写错。
module mldsa_decompose (
    input  wire               mode,   // 0 = (q−1)/88，1 = (q−1)/32
    input  wire signed [31:0] a,
    output wire signed [31:0] a0,
    output wire        [5:0]  a1
);
    localparam signed [31:0] Q     = 32'sd8380417;
    localparam signed [31:0] QHALF = 32'sd4190208;   // (q−1)/2

    wire [23:0] a1t_full = (a[23:0] + 24'd127) >> 7;
    wire [16:0] a1t      = a1t_full[16:0];

    // γ₂=(q−1)/88 那一支
    wire [30:0] scaled88 = a1t * 17'd11275 + 31'd8388608;
    wire [6:0]  quot88   = scaled88[30:24];
    // 参考实现用 a1 ^= ((43 − a1) >> 31) & a1 把 44 钳到 0；
    // 硬件里选择器与数据无关，直接写条件形式。
    wire [5:0]  a1_88    = (quot88 > 7'd43) ? 6'd0 : quot88[5:0];

    // γ₂=(q−1)/32 那一支
    wire [27:0] scaled32 = a1t * 17'd1025 + 28'd2097152;
    wire [5:0]  a1_32    = {2'd0, scaled32[25:22]};

    assign a1 = mode ? a1_32 : a1_88;

    // ⚠️ **不要写成 a1 * gamma2x2**：γ₂ 改成运行时信号之后那就是一个真的
    //    32×32 乘法器，而原来（γ₂ 是常量）综合器把它变成几次移位相加。
    //    实测代价：verify 的 WNS 从 +0.993 掉到 −0.893，engine 一起掉。
    //    a1 只有 6 位、γ₂ 只有两个取值 —— 两个**常量乘**算完再选，
    //    综合器仍然能各自优化成移位相加。
    wire signed [31:0] prod88 = $signed({26'd0, a1}) * 32'sd190464;
    wire signed [31:0] prod32 = $signed({26'd0, a1}) * 32'sd523776;
    wire signed [31:0] sub = a - (mode ? prod32 : prod88);
    // a₀ 超过 (q−1)/2 时减一个 q，折回对称区间
    assign a0 = (sub > QHALF) ? (sub - Q) : sub;
endmodule

module mldsa_make_hint (
    input  wire               mode,
    input  wire signed [31:0] a0,
    input  wire        [5:0]  a1,
    output wire               hint
);
    wire signed [31:0] GAMMA2 = mode ? 32'sd261888 : 32'sd95232;

    assign hint = (a0 > GAMMA2) || (a0 < -GAMMA2)
               || ((a0 == -GAMMA2) && (a1 != 6'd0));
endmodule

module mldsa_use_hint (
    input  wire               mode,
    input  wire signed [31:0] a,
    input  wire               hint,
    output wire        [5:0]  a1_out
);
    wire signed [31:0] a0;
    wire        [5:0]  a1;
    mldsa_decompose u_dec (.mode(mode), .a(a), .a0(a0), .a1(a1));

    wire up = (a0 > 32'sd0);

    // 高位的循环域随 γ₂ 变：(q−1)/88 时在 [0,44)，(q−1)/32 时在 [0,16)
    wire [5:0] inc88 = (a1 == 6'd43) ? 6'd0  : (a1 + 6'd1);
    wire [5:0] dec88 = (a1 == 6'd0)  ? 6'd43 : (a1 - 6'd1);
    wire [5:0] inc32 = {2'd0, (a1[3:0] + 4'd1)};
    wire [5:0] dec32 = {2'd0, (a1[3:0] - 4'd1)};
    wire [5:0] inc   = mode ? inc32 : inc88;
    wire [5:0] dec   = mode ? dec32 : dec88;

    assign a1_out = !hint ? a1 : (up ? inc : dec);
endmodule

`default_nettype wire
