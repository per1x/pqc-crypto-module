// mlkem_bitpack / mlkem_bitunpack —— 变宽度的 ByteEncode_d / ByteDecode_d
//
// FIPS 203 的 ByteEncode_d 把每个系数的低 d 位按**低位在前**接进一条比特流，
// 再按低位在前切成字节。ML-KEM 用到 d ∈ {1, 4, 5, 10, 11}（还有 12，但 12 位
// 那一路两个系数正好三字节，pack.v 里已经有纯组合的 encode12/decode12 了）。
//
// 【为什么做成运行时可变的 d，而不是每个 d 例化一份】
// `param_set` 是运行时选的：512/768 用 du=10、dv=4，1024 用 du=11、dv=5。
// 同一块板要同时支持三套参数，所以 d 必须是输入而不是参数。
// 代价是移位量变成变量 —— 但 acc 只有 20 位，变量移位综合出来是几十个 LUT。
//
// 【为什么不需要 flush】
// ML-KEM 每个多项式是 256 个系数，256·d 比特 = 32d 字节，**总是整字节**。
// 所以比特累加器每跑完一个多项式必然回到空，不存在"最后剩几个比特"的收尾。
// 上层要换 d 时在两个多项式之间换即可。
//
// 【输入与输出互斥地推进】
// in_ready 与 out_valid 的条件互补（一个看 nbits 少、一个看 nbits 多），
// 所以同一拍不会既收又发。这样写吞吐是每拍一个字节或一个系数，
// 对原型验证够用，而且省掉了"同拍收发"那一类最容易写错的边界。
`default_nettype none

// 系数流 → 字节流
module mlkem_bitpack (
    input  wire        clk,
    input  wire        rst_n,
    input  wire [3:0]  d,            // 1..12，只在 in_valid 为低时可以改

    input  wire        in_valid,
    output wire        in_ready,
    input  wire [11:0] in_data,      // 只取低 d 位

    output wire        out_valid,
    input  wire        out_ready,
    output wire [7:0]  out_data
);
    // 收之前 nbits ≤ 7，加上最多 12 位 = 19，20 位够
    reg [19:0] acc;
    reg [4:0]  nbits;

    assign in_ready  = (nbits < 5'd8);
    assign out_valid = (nbits >= 5'd8);
    assign out_data  = acc[7:0];

    wire [19:0] masked = {20{1'b1}} >> (5'd20 - {1'b0, d});   // 低 d 位的掩码

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            acc   <= 20'd0;
            nbits <= 5'd0;
        end else if (in_valid && in_ready) begin
            acc   <= acc | (({8'd0, in_data} & masked) << nbits);
            nbits <= nbits + {1'b0, d};
        end else if (out_valid && out_ready) begin
            acc   <= acc >> 8;
            nbits <= nbits - 5'd8;
        end
    end
endmodule

// 字节流 → 系数流
module mlkem_bitunpack (
    input  wire        clk,
    input  wire        rst_n,
    input  wire [3:0]  d,

    input  wire        in_valid,
    output wire        in_ready,
    input  wire [7:0]  in_data,

    output wire        out_valid,
    input  wire        out_ready,
    output wire [11:0] out_data
);
    // 收之前 nbits ≤ d−1 ≤ 11，加 8 = 19，同样 20 位
    reg [19:0] acc;
    reg [4:0]  nbits;

    assign in_ready  = (nbits < {1'b0, d});
    assign out_valid = (nbits >= {1'b0, d});

    wire [19:0] masked = {20{1'b1}} >> (5'd20 - {1'b0, d});
    assign out_data = acc[11:0] & masked[11:0];

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            acc   <= 20'd0;
            nbits <= 5'd0;
        end else if (in_valid && in_ready) begin
            acc   <= acc | ({12'd0, in_data} << nbits);
            nbits <= nbits + 5'd8;
        end else if (out_valid && out_ready) begin
            acc   <= acc >> d;
            nbits <= nbits - {1'b0, d};
        end
    end
endmodule

`default_nettype wire
