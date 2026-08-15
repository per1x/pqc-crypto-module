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


// ============================================================================
// mldsa_mont_mul_pipe —— mont(x·y) 的流水版本，**全工程唯一的流水乘法链**
// ============================================================================
//
// 上面那个组合模块把三次 32×32 乘法（x·y、m=p·QINV、m·Q）串在一拍里。
// ML-DSA-87 Sign 的 post-route 实测证明这条链撑不住 100MHz：
//   蝶形那条 WNS = −3.332ns；把蝶形流水化之后关键路径原地搬到 sign.v 的
//   逐点乘 pw_prod 上，WNS = −3.356ns —— 同一个形状的链，换了个位置而已。
// 所以这类链必须**在一个地方**切好，然后各处共用，而不是每处各切一次。
//
// 五级：
//   1  x/y 打进寄存器（正好落到 DSP48E2 的 A/B 输入寄存器）
//   2  p = x·y
//   3  m = (int32)(p[31:0]·QINV)
//   4  mq = m·Q
//   5  t = (p − mq) >>> 32，**出口也是寄存器**（直接进 BRAM 的 DIN）
//
// in_tag 是跟着数据走的透明通道：写回地址、旁路值、模式位都塞它里面，
// 于是"数据到了、伴随信号却错位"这类 bug 在结构上就不可能发生。
// mldsa_butterfly_pipe 正是靠它把蝶形的旁路 a/a+b 与 t 对齐的。
//
// ⚠️ 数据通路寄存器**一律不带复位**：DSP48E2 的内部流水寄存器只支持同步复位，
// 挂上异步复位 Vivado 就无法把它们塞进 DSP，流水白打。
// 复位只加在 valid/tag 这条控制链上（本来就是 fabric 触发器）。
module mldsa_mont_mul_pipe #(
    parameter integer TAGW = 8
) (
    input  wire               clk,
    input  wire               rst_n,

    input  wire               in_valid,
    input  wire [TAGW-1:0]    in_tag,
    input  wire signed [31:0] x,
    input  wire signed [31:0] y,

    output wire               out_valid,
    output wire [TAGW-1:0]    out_tag,
    output wire signed [31:0] t_out,

    // 流水线里还有没有在飞的运算。调用方换段前要靠它排空。
    output wire               pipe_busy
);
    // 从 in_valid 到 out_valid 的拍数。
    // ⚠️ 这是**描述**下面数据通路的级数，不是可调的旋钮 —— 改它不会自动多切一级。
    localparam integer LATENCY = 5;

    localparam signed [31:0] Q    = 32'sd8380417;
    localparam signed [31:0] QINV = 32'sd58728449;   // q⁻¹ mod 2³²

    // ---- 控制链（带复位）----
    reg [LATENCY-1:0] v_p;
    reg [TAGW-1:0]    tag_p [1:LATENCY];

    integer i;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            v_p <= {LATENCY{1'b0}};
            for (i = 1; i <= LATENCY; i = i + 1) tag_p[i] <= {TAGW{1'b0}};
        end else begin
            v_p <= {v_p[LATENCY-2:0], in_valid};
            tag_p[1] <= in_tag;
            for (i = 2; i <= LATENCY; i = i + 1) tag_p[i] <= tag_p[i-1];
        end
    end

    // ---- 数据通路（**不带复位**，见上面的 ⚠️）----
    reg signed [31:0] x_s1, y_s1;
    reg signed [63:0] p_s2, p_s3, p_s4;
    reg signed [31:0] m_s3;
    reg signed [63:0] mq_s4;
    reg signed [31:0] t_s5;

    initial begin
        x_s1 = 32'sd0; y_s1 = 32'sd0;
        p_s2 = 64'sd0; p_s3 = 64'sd0; p_s4 = 64'sd0;
        m_s3 = 32'sd0; mq_s4 = 64'sd0; t_s5 = 32'sd0;
    end

    // 级 1：操作数进寄存器
    always @(posedge clk) begin
        x_s1 <= x;
        y_s1 <= y;
    end

    // 级 2：x·y。位宽显式写满 —— 与上面组合版的注释同一个理由：
    // 那个 2-state 的仿真器会把隐式截断真的算错。
    always @(posedge clk)
        p_s2 <= $signed({{32{x_s1[31]}}, x_s1}) * $signed({{32{y_s1[31]}}, y_s1});

    // 级 3：m = (int32_t)a * QINV —— 32 位上下文，自然截断到低 32 位。
    // 这一步的截断是 Montgomery 约减能成立的前提，不能扩宽。
    always @(posedge clk) begin
        m_s3 <= $signed(p_s2[31:0]) * QINV;
        p_s3 <= p_s2;
    end

    // 级 4：m·Q
    always @(posedge clk) begin
        mq_s4 <= $signed({{32{m_s3[31]}}, m_s3}) * $signed({{32{Q[31]}}, Q});
        p_s4  <= p_s3;
    end

    // 级 5：(a − m·Q) >>> 32，即取高 32 位
    wire signed [63:0] diff_s4 = p_s4 - mq_s4;
    always @(posedge clk) t_s5 <= diff_s4[63:32];

    assign t_out     = t_s5;
    assign out_valid = v_p[LATENCY-1];
    assign out_tag   = tag_p[LATENCY];
    assign pipe_busy = |v_p;
endmodule

`default_nettype wire
