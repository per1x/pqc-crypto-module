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

module mldsa_decompose #(
    parameter integer MODE = 0
) (
    input  wire signed [31:0] a,
    output wire signed [31:0] a0,
    output wire        [5:0]  a1
);
    localparam signed [31:0] Q        = 32'sd8380417;
    localparam signed [31:0] GAMMA2X2 = (MODE == 0) ? 32'sd190464 : 32'sd523776;
    localparam signed [31:0] QHALF    = 32'sd4190208;   // (q−1)/2

    wire [23:0] a1t_full = (a[23:0] + 24'd127) >> 7;
    wire [16:0] a1t      = a1t_full[16:0];

    wire [5:0] a1_raw;
    generate
        if (MODE == 0) begin : g_88
            wire [30:0] scaled = a1t * 17'd11275 + 31'd8388608;
            wire [6:0]  quot   = scaled[30:24];
            // 参考实现用 a1 ^= ((43 − a1) >> 31) & a1 把 44 钳到 0；
            // 硬件里选择器与数据无关，直接写条件形式。
            assign a1_raw = (quot > 7'd43) ? 6'd0 : quot[5:0];
        end else begin : g_32
            wire [27:0] scaled = a1t * 17'd1025 + 28'd2097152;
            assign a1_raw = {2'd0, scaled[25:22]};
        end
    endgenerate

    assign a1 = a1_raw;

    wire signed [31:0] sub = a - $signed({26'd0, a1}) * GAMMA2X2;
    // a₀ 超过 (q−1)/2 时减一个 q，折回对称区间
    assign a0 = (sub > QHALF) ? (sub - Q) : sub;
endmodule

module mldsa_make_hint #(
    parameter integer MODE = 0
) (
    input  wire signed [31:0] a0,
    input  wire        [5:0]  a1,
    output wire               hint
);
    localparam signed [31:0] GAMMA2 = (MODE == 0) ? 32'sd95232 : 32'sd261888;

    assign hint = (a0 > GAMMA2) || (a0 < -GAMMA2)
               || ((a0 == -GAMMA2) && (a1 != 6'd0));
endmodule

module mldsa_use_hint #(
    parameter integer MODE = 0
) (
    input  wire signed [31:0] a,
    input  wire               hint,
    output wire        [5:0]  a1_out
);
    wire signed [31:0] a0;
    wire        [5:0]  a1;
    mldsa_decompose #(.MODE(MODE)) u_dec (.a(a), .a0(a0), .a1(a1));

    wire up = (a0 > 32'sd0);

    generate
        if (MODE == 0) begin : g_88
            // 高位在 [0, 44) 上循环
            wire [5:0] inc = (a1 == 6'd43) ? 6'd0  : (a1 + 6'd1);
            wire [5:0] dec = (a1 == 6'd0)  ? 6'd43 : (a1 - 6'd1);
            assign a1_out = !hint ? a1 : (up ? inc : dec);
        end else begin : g_32
            // 高位在 [0, 16) 上循环，掩码即可
            wire [5:0] inc = {2'd0, (a1[3:0] + 4'd1)};
            wire [5:0] dec = {2'd0, (a1[3:0] - 4'd1)};
            assign a1_out = !hint ? a1 : (up ? inc : dec);
        end
    endgenerate
endmodule

`default_nettype wire
