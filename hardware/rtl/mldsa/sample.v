// mldsa_rej_uniform / mldsa_rej_eta / mldsa_rej_uniform_buf —— ML-DSA 的两类拒绝采样
//
// 【均匀采样 RejNTTPoly（FIPS 204 Alg 30）】
// 从 SHAKE128 流里每 3 字节取出一个 23 位候选（最高位丢掉），小于 q 的收下。
// 与 ML-KEM 的 12 位候选不同：ML-DSA 的系数是 23 位，一个候选正好吃 3 字节。
//
// 【有界采样 RejBoundedPoly（FIPS 204 Alg 31）】
// 每字节拆成两个 4 位半字节，映射到 [−η, η]：
//   η = 2：半字节 < 15 才收，先把 [0,15) 折到 [0,5)（乘倒数代替对 5 取模），再算 2 − t
//   η = 4：半字节 < 9  才收，直接算 4 − t
//
// 丢弃与否只取决于 XOF 输出：均匀采样的输入是公开的 ρ，与私密数据无关；
// 有界采样的输入虽由私密种子派生，但拒绝模式只泄露 XOF 流本身的统计，
// 不泄露最终系数 —— 这与参考实现的取舍一致。
`default_nettype none

module mldsa_rej_uniform (
    input  wire [23:0] bytes_in,     // 3 字节，bytes_in[7:0] 是最低地址那一字节
    output wire [22:0] cand,
    output wire        cand_ok
);
    localparam [22:0] Q = 23'd8380417;

    assign cand    = bytes_in[22:0];  // 等价于 t & 0x7FFFFF
    assign cand_ok = (cand < Q);
endmodule

// ⚠️ η 是**运行时输入**（运行时选 44/65/87 的一部分）：
//    44/87 用 η=2，65 用 η=4，两支的接受门限与折算式都不同。
//    两支并行算完再选 —— 都是几个比较器和一次 4 位乘法，不值得为省这点面积
//    去把两套式子揉成一条。
module mldsa_rej_eta (
    input  wire        [2:0]  eta,      // 2 或 4
    input  wire        [3:0]  nibble,
    output wire signed [31:0] coeff,
    output wire               coeff_ok
);
    // η=2 支：205·t >> 10 等价于 floor(t/5)，t < 15
    wire [11:0] scaled = nibble * 8'd205;
    wire [3:0]  fifth  = {2'd0, scaled[11:10]};
    wire [3:0]  rem    = nibble - (fifth * 4'd5);
    wire        ok2    = (nibble < 4'd15);
    wire signed [31:0] c2 = 32'sd2 - $signed({28'd0, rem});

    // η=4 支
    wire        ok4    = (nibble < 4'd9);
    wire signed [31:0] c4 = 32'sd4 - $signed({28'd0, nibble});

    wire is2 = (eta == 3'd2);
    assign coeff_ok = is2 ? ok2 : ok4;
    assign coeff    = is2 ? c2  : c4;
endmodule

// 均匀采样的收集器：每周期吃一组 3 字节，攒够 256 个系数就置 done。
//
// 接口与 mlkem_rej_uniform 同形（start 触发 → 轮询 done → 读结果），
// 输入侧用 valid/ready 握手，因为字节流由上游 SHAKE 核按拍供给。
//
// ⚠️ **读口是同步读**：给出 rd_addr 之后要走一个时钟沿，rd_data 才是那个
// 地址的内容。原来是组合读，但组合读的存储在 Vivado 里**永远变不成 BRAM**
// —— 它会被摊成 F7/F8 多路选择器树，256×23 bit 要吃掉几千个 LUT。
// 换成 common/ram_dp 之后是 1 块 BRAM。详见 rtl/common/ram_dp.v 的注释。
module mldsa_rej_uniform_buf (
    input  wire        clk,
    input  wire        rst_n,

    input  wire        start,
    output reg         done,

    input  wire        in_valid,
    input  wire [23:0] in_bytes,
    output wire        in_ready,

    output reg  [8:0]  count,
    input  wire [7:0]  rd_addr,
    output wire [22:0] rd_data
);
    reg        busy;

    assign in_ready = busy;

    wire [22:0] cand;
    wire        cand_ok;
    mldsa_rej_uniform u_cand (
        .bytes_in(in_bytes), .cand(cand), .cand_ok(cand_ok));

    wire take = cand_ok && (count < 9'd256);

    // 写口（A）在采样期间用 count 当地址，读口（B）常年挂 rd_addr。
    // 采样和读结果在时间上不重叠（done 之前不该读），所以两个口不会打架。
    ram_dp #(.DW(23), .AW(8)) u_mem (
        .clk(clk),
        .a_we(busy && in_valid && take), .a_addr(count[7:0]),
        .a_din(cand), .a_dout(),
        .b_we(1'b0), .b_addr(rd_addr), .b_din(23'd0), .b_dout(rd_data));

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
            if (take) begin
                count           <= count + 9'd1;
                if (count == 9'd255) begin
                    busy <= 1'b0;
                    done <= 1'b1;
                end
            end
        end
    end
endmodule

`default_nettype wire
