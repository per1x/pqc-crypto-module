// ML-DSA 的三种系数打包（FIPS 204 §7.1）
//
// ============================================================================
// 【为什么打包要单独做成流式模块】
// ============================================================================
// 三种打包的位宽都不是 8 的因数（10 位 / 13 位 / 3 位），所以每个输出字节都
// 由**相邻两个甚至三个系数**的片段拼成。写成"一个系数进、若干位出"的组合
// 电路会得到一堆宽度各异的移位拼接，改一处就要重新数一遍所有位。
//
// 这里统一成同一个形状：**系数一个个进来，字节一个个出去**，内部只有一个
// 位累加器。三个模块的区别只剩"每个系数占几位"和"进来之前怎么变换"。
// 于是每个模块的正确性只依赖两件事：宽度对不对、变换对不对 ——
// 都是能一眼看完的东西。
//
// ============================================================================
// 【那个"先减一下"的变换不是可选项】
// ============================================================================
// t₀ 存的是 2^(D−1) − a，s₁/s₂ 存的是 η − a。这不是压缩技巧，是把有符号数
// 搬进无符号区间：t₀ 的取值域是 (−2^(D−1), 2^(D−1)]，直接按 13 位无符号打包
// 会把负数打成一个巨大的正数，解包回来全错。
//
// 而这类错误**在自测里看不出来**：打包→解包能对上（两次错误互相抵消），
// 只有跟标准向量比才会暴露。所以这三个模块一律对着 ACVP 的 pk/sk 字节比，
// 不跟自己的解包比。
`default_nettype none

// ---------------------------------------------------------------------------
// 通用位打包器：每个系数 W 位，低位在前，攒够 8 位吐一个字节
// ---------------------------------------------------------------------------
module mldsa_bitpack #(
    parameter integer W  = 10,        // 每个系数占的位数
    parameter integer IW = 13         // in_val 端口宽度（z 打包 W=18 时要放宽到 18）
) (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        clr,           // 清空累加器（每条多项式开始前拉一拍）

    input  wire [IW-1:0] in_val,      // 已经做完变换的无符号值，只看低 W 位
    input  wire        in_valid,
    output wire        in_ready,      // 累加器满 8 位时拉低（见下面的说明）

    output reg  [7:0]  out_byte,
    output reg         out_valid
);
    // 累加器要装得下"接受一个新系数之前的残留" + "这一拍进来的 W 位"。
    // 残留最多 7 位（8 位以上会先被吐出去），所以 W+7 位就够，取 W+8 留一位余量。
    reg [W+8-1:0] acc;
    reg [4:0]     nbits;              // 累加器里现在有多少位有效

    // ⚠️ **必须反压。** 第一版写的是 in_ready 恒为 1、只在没有新系数的拍次
    //    才吐字节 —— 那是错的：W=10 时每拍进 10 位、最多出 8 位，
    //    连续喂的话累加器只涨不落，最后高位被截掉，打出来的字节从中间开始错。
    //    这种错很阴：前几个字节是对的，所以短向量能过。
    //
    //    改成"累加器里不足 8 位时才收新系数"，够 8 位就先吐。代价是吞吐降到
    //    每 2~3 拍一个系数 —— 打包是控制面速度的事，正确性远比吞吐要紧。
    assign in_ready = (nbits < 5'd8);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            acc <= {(W+8){1'b0}};
            nbits <= 5'd0;
            out_byte <= 8'd0;
            out_valid <= 1'b0;
        end else begin
            out_valid <= 1'b0;

            if (clr) begin
                acc <= {(W+8){1'b0}};
                nbits <= 5'd0;
            end else if (in_valid && in_ready) begin
                // 新系数拼到高位侧：低位在前的约定就是这一句。
                acc <= acc | ({{(W+8-W){1'b0}}, in_val[W-1:0]} << nbits);
                nbits <= nbits + W[4:0];
            end else if (nbits >= 5'd8) begin
                out_byte  <= acc[7:0];
                out_valid <= 1'b1;
                acc       <= acc >> 8;
                nbits     <= nbits - 5'd8;
            end
        end
    end

    // 三种打包的总位数都是 8 的整数倍（256×10、256×13、256×3、256×4），
    // 所以一条多项式打完 nbits 必然回到 0 —— 不需要收尾冲刷。
    // 这一点值得写下来：如果哪天加了一种位宽让它不成立，
    // 少掉的是最后那几位，而多项式的开头全对，测试很容易看不出来。
endmodule

// ---------------------------------------------------------------------------
// t₁：每系数 10 位，直接打，没有变换（t₁ 本来就在 [0, 2^10)）
// ---------------------------------------------------------------------------
module mldsa_polyt1_pack (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        clr,
    input  wire [9:0]  coef,
    input  wire        in_valid,
    output wire        in_ready,
    output wire [7:0]  out_byte,
    output wire        out_valid
);
    mldsa_bitpack #(.W(10)) u_bp (
        .clk(clk), .rst_n(rst_n), .clr(clr),
        .in_val({3'd0, coef}), .in_valid(in_valid), .in_ready(in_ready),
        .out_byte(out_byte), .out_valid(out_valid));
endmodule

// ---------------------------------------------------------------------------
// t₀：每系数 13 位，先换成 2^(D−1) − a
//
// 这个减法是把有符号搬进无符号：t₀ ∈ (−2^(D−1), 2^(D−1)]，
// 不减直接按 13 位无符号打，负数会变成一个巨大的正数。
// 而**打包→解包仍然对得上**（两次错误互相抵消），只有跟标准向量比才暴露。
// ---------------------------------------------------------------------------
module mldsa_polyt0_pack #(
    parameter integer D = 13
) (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        clr,
    input  wire signed [12:0] coef,     // 有符号
    input  wire        in_valid,
    output wire        in_ready,
    output wire [7:0]  out_byte,
    output wire        out_valid
);
    // 2^(D−1) − a。**由 D 算出来，不写死常量** —— 第一版写的是 13'sd4096，
    // 参数 D 于是成了摆设：改 D 不会改行为，只会让常量和参数不一致，
    // 而这种不一致编译器不会说话（是 Verilator 的 UNUSEDPARAM 抓到的）。
    localparam [12:0] HALF = 13'd1 << (D - 1);
    wire [12:0] shifted = HALF - coef;
    mldsa_bitpack #(.W(13)) u_bp (
        .clk(clk), .rst_n(rst_n), .clr(clr),
        .in_val(shifted), .in_valid(in_valid), .in_ready(in_ready),
        .out_byte(out_byte), .out_valid(out_valid));
endmodule

// ---------------------------------------------------------------------------
// s₁/s₂：η=2 时每系数 3 位，η=4 时 4 位；都先换成 η − a
//
// 位宽随 η 变，所以做成参数而不是运行时选择：一个 bitstream 里两种参数集
// 各例化一份，比让位宽变成运行时信号简单得多，也没有额外代价 ——
// 这几个模块小到可以忽略。
// ---------------------------------------------------------------------------
module mldsa_polyeta_pack #(
    parameter integer ETA = 2,
    parameter integer W   = (ETA == 2) ? 3 : 4
) (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        clr,
    input  wire signed [12:0] coef,
    input  wire        in_valid,
    output wire        in_ready,
    output wire [7:0]  out_byte,
    output wire        out_valid
);
    wire [12:0] shifted = ETA[12:0] - coef;
    mldsa_bitpack #(.W(W)) u_bp (
        .clk(clk), .rst_n(rst_n), .clr(clr),
        .in_val(shifted), .in_valid(in_valid), .in_ready(in_ready),
        .out_byte(out_byte), .out_valid(out_valid));
endmodule
