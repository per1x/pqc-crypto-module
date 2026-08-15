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
// ⚠️ 位宽 W 是**运行时输入**，不是参数。
//    原来它是编译期参数，注释里写着"一个 bitstream 里两种参数集各例化一份"——
//    那个前提已经不成立了：任务要求**同一个 bitstream 运行时可选 44/65/87**，
//    而 s₁/s₂ 的位宽随 η 变（3 或 4）、z 随 γ₁ 变（18 或 20）、w₁ 随 γ₂ 变（6 或 4）。
//    再走"各例化一份"就等于把三套参数集的打包器全放进去，
//    而且调用方还得按运行时的 pset 去选 —— 那才是真的浪费与复杂。
//    改成运行时之后端口按**最宽的 20 位**开，代价只有几个比较器。
module mldsa_bitpack (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        clr,           // 清空累加器（每条多项式开始前拉一拍）

    input  wire [4:0]  w,             // 每个系数占的位数，运行时给（3…20）
    input  wire [19:0] in_val,        // 已经做完变换的无符号值，只看低 w 位
    input  wire        in_valid,
    output wire        in_ready,      // 累加器满 8 位时拉低（见下面的说明）

    output reg  [7:0]  out_byte,
    output reg         out_valid
);
    // 累加器要装得下"接受一个新系数之前的残留" + "这一拍进来的 w 位"。
    // 残留最多 7 位（8 位以上会先被吐出去），w 最大 20，所以 27 位就够，取 28。
    reg [27:0] acc;
    reg [4:0]  nbits;                 // 累加器里现在有多少位有效（最大 7+20=27）

    // 只取低 w 位。原来这是静态位选 in_val[W-1:0]，运行时要显式造掩码。
    // 掩码在 21 位里算：w=20 时 (1<<20) 在 20 位里会溢出成 0。
    wire [20:0] w_mask = (21'd1 << w) - 21'd1;
    wire [19:0] masked = in_val & w_mask[19:0];

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
            acc <= 28'd0;
            nbits <= 5'd0;
            out_byte <= 8'd0;
            out_valid <= 1'b0;
        end else begin
            out_valid <= 1'b0;

            if (clr) begin
                acc <= 28'd0;
                nbits <= 5'd0;
            end else if (in_valid && in_ready) begin
                // 新系数拼到高位侧：低位在前的约定就是这一句。
                acc <= acc | ({8'd0, masked} << nbits);
                nbits <= nbits + w;
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
    // t₁ 的 10 位不随参数集变，w 直接给常量
    mldsa_bitpack u_bp (
        .clk(clk), .rst_n(rst_n), .clr(clr), .w(5'd10),
        .in_val({10'd0, coef}), .in_valid(in_valid), .in_ready(in_ready),
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
    // t₀ 的 13 位不随参数集变（D 恒为 13），w 直接给常量
    mldsa_bitpack u_bp (
        .clk(clk), .rst_n(rst_n), .clr(clr), .w(5'd13),
        .in_val({7'd0, shifted}), .in_valid(in_valid), .in_ready(in_ready),
        .out_byte(out_byte), .out_valid(out_valid));
endmodule

// ---------------------------------------------------------------------------
// s₁/s₂：η=2 时每系数 3 位，η=4 时 4 位；都先换成 η − a
//
// ⚠️ η 是**运行时输入**。原来它是编译期参数，注释里的理由是"一个 bitstream 里
//    两种参数集各例化一份"—— 那个前提已被"运行时可选 44/65/87"推翻：
//    真按那样做，调用方还得按运行时的 pset 在两份之间选，比直接运行时化更繁。
// ---------------------------------------------------------------------------
module mldsa_polyeta_pack (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        clr,
    input  wire [2:0]  eta,             // 2 或 4
    input  wire signed [12:0] coef,
    input  wire        in_valid,
    output wire        in_ready,
    output wire [7:0]  out_byte,
    output wire        out_valid
);
    wire [12:0] shifted = {10'd0, eta} - coef;
    wire [4:0]  w       = (eta == 3'd2) ? 5'd3 : 5'd4;
    mldsa_bitpack u_bp (
        .clk(clk), .rst_n(rst_n), .clr(clr), .w(w),
        .in_val({7'd0, shifted}), .in_valid(in_valid), .in_ready(in_ready),
        .out_byte(out_byte), .out_valid(out_valid));
endmodule
