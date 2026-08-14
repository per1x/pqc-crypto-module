// mlkem_cbd2 / mlkem_cbd3 / mlkem_rej_pair —— ML-KEM 的两类采样
//
// 【中心二项分布 CBD（FIPS 203 Alg 8）】
// 定义是：取 2η 个比特，前 η 个的汉明重量减后 η 个的汉明重量。
// 直接按定义做需要逐比特累加；硬件与参考实现都用同一个位并行技巧：
// 把"每 η 位一组求和"变成若干次带掩码的加法，一次算出一整批系数。
//
//   η = 2：4 字节 → 8 个系数。掩码 0x55555555，两次累加后每 2 位是一个计数。
//   η = 3：3 字节 → 4 个系数。掩码 0x00249249，三次累加后每 3 位是一个计数。
//
// 位并行技巧与逐比特定义是否等价，由 hardware/tb/cocotb/test_sample.py 对着
// hardware/model/mlkem_oracle.py 里按 FIPS 203 定义逐比特实现的版本验证；
// η = 3 的输入域只有 2^24，测试在随机抽样之外还覆盖了全 0/全 1 等边界。
//
// 【拒绝采样 SampleNTT（FIPS 203 Alg 7）】
// 从 SHAKE128 流里每 3 字节取出两个 12 位候选，小于 q 的收下，否则丢弃：
//     d1 = b0       | (b1 & 0x0F) << 8
//     d2 = (b1 >> 4) | b2 << 4
// mlkem_rej_pair 是这一步的组合逻辑；mlkem_rej_uniform 在它之上做收集，
// 每周期吃一组 3 字节，直到攒够 256 个系数。
//
// 丢弃与否只取决于字节流本身、与私密数据无关，所以这里的数据相关分支
// 不构成侧信道 —— 公开矩阵 Â 的采样输入是公开的 ρ。
`default_nettype none

module mlkem_cbd2 (
    input  wire [31:0] rand_in,      // 4 字节，rand_in[7:0] 是最低地址那一字节
    output wire [127:0] coeffs       // 8 个 16 位有符号系数，低位在前
);
    localparam [31:0] MASK = 32'h55555555;

    wire [31:0] d = (rand_in & MASK) + ((rand_in >> 1) & MASK);

    genvar i;
    generate
        for (i = 0; i < 8; i = i + 1) begin : g_cbd2
            wire [1:0] a = d[4 * i + 1 -: 2];
            wire [1:0] b = d[4 * i + 3 -: 2];
            assign coeffs[16 * i + 15 -: 16] =
                $signed({14'd0, a}) - $signed({14'd0, b});
        end
    endgenerate
endmodule

module mlkem_cbd3 (
    input  wire [23:0] rand_in,      // 3 字节
    output wire [63:0] coeffs        // 4 个 16 位有符号系数
);
    localparam [23:0] MASK = 24'h249249;

    wire [23:0] d = (rand_in & MASK)
                  + ((rand_in >> 1) & MASK)
                  + ((rand_in >> 2) & MASK);

    genvar i;
    generate
        for (i = 0; i < 4; i = i + 1) begin : g_cbd3
            wire [2:0] a = d[6 * i + 2 -: 3];
            wire [2:0] b = d[6 * i + 5 -: 3];
            assign coeffs[16 * i + 15 -: 16] =
                $signed({13'd0, a}) - $signed({13'd0, b});
        end
    endgenerate
endmodule

module mlkem_rej_pair (
    input  wire [23:0] bytes_in,     // 3 字节，bytes_in[7:0] 是最低地址那一字节
    output wire [11:0] d1,
    output wire [11:0] d2,
    output wire        d1_ok,
    output wire        d2_ok
);
    localparam [11:0] Q = 12'd3329;

    assign d1 = {bytes_in[11:8], bytes_in[7:0]};
    assign d2 = {bytes_in[23:16], bytes_in[15:12]};

    assign d1_ok = (d1 < Q);
    assign d2_ok = (d2 < Q);
endmodule

// 收集器：每周期吃一组 3 字节，把通过的候选写进系数存储，攒够 256 个就置 done。
//
// 接口沿用本仓库其余核的形状（start 触发 → 轮询 done → 组合读结果），
// 输入侧用 valid/ready 握手，因为字节流由上游 SHAKE 核按拍供给。
module mlkem_rej_uniform (
    input  wire        clk,
    input  wire        rst_n,

    input  wire        start,        // 单周期脉冲：清空计数，开始收集
    output reg         done,         // 电平语义：置位后保持到下一次 start

    input  wire        in_valid,
    input  wire [23:0] in_bytes,
    output wire        in_ready,     // 收集未完成时恒为 1

    output reg  [8:0]  count,        // 已收下的系数个数
    input  wire [7:0]  rd_addr,
    output wire [15:0] rd_data
);
    reg        busy;

    assign in_ready = busy;

    wire [11:0] d1, d2;
    wire        d1_ok, d2_ok;
    mlkem_rej_pair u_pair (
        .bytes_in(in_bytes), .d1(d1), .d2(d2), .d1_ok(d1_ok), .d2_ok(d2_ok));

    // 本拍实际能收下的候选：第二个还要看第一个收下后是否已满
    wire take1 = d1_ok && (count < 9'd256);
    wire take2 = d2_ok && ((count + (take1 ? 9'd1 : 9'd0)) < 9'd256);

    wire [8:0] slot2 = count + (take1 ? 9'd1 : 9'd0);
    wire [8:0] next  = slot2 + (take2 ? 9'd1 : 9'd0);

    // 系数存储用一块真双口 BRAM，而不是 256×12 的寄存器阵列：
    // 后者有两个写口（同一拍最多收下两个候选）加一个组合读口，
    // 综合出来是几千个 LUT 的选择树，理由与 ntt_core 那次一样，见 docs/TESTING.md 的 S3。
    //
    // 端口分工：A 口专写第一个候选；B 口在收集期间写第二个候选，
    // 收完（!busy）之后改接外部读口。两个候选的槽位恒差 1，不会同址。
    // ⚠️ 读口因此是**同步读**：给出 rd_addr 要等一个上升沿。
    wire        m_a_we   = busy && in_valid && take1;
    wire        m_b_we   = busy && in_valid && take2;
    wire [7:0]  m_b_addr = busy ? slot2[7:0] : rd_addr;
    wire [11:0] m_b_dout;

    ram_dp #(.DW(12), .AW(8)) u_mem (
        .clk    (clk),
        .a_we   (m_a_we), .a_addr(count[7:0]), .a_din(d1), .a_dout(),
        .b_we   (m_b_we), .b_addr(m_b_addr),   .b_din(d2), .b_dout(m_b_dout)
    );

    assign rd_data  = {4'd0, m_b_dout};

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            done  <= 1'b0;
            busy  <= 1'b0;
            count <= 9'd0;
        end else if (start) begin
            done  <= 1'b0;
            busy  <= 1'b1;
            count <= 9'd0;
        end else if (busy && in_valid) begin
            // 两个候选的写入由上面的 BRAM 端口直接完成，这里只推进计数
            count <= next;
            if (next == 9'd256) begin
                busy <= 1'b0;
                done <= 1'b1;
            end
        end
    end
endmodule

`default_nettype wire
