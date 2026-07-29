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

module mldsa_rej_eta #(
    parameter integer ETA = 2
) (
    input  wire        [3:0]  nibble,
    output wire signed [31:0] coeff,
    output wire               coeff_ok
);
    generate
        if (ETA == 2) begin : g_eta2
            // 205·t >> 10 等价于 floor(t/5)，t < 15
            wire [11:0] scaled = nibble * 8'd205;
            wire [3:0]  fifth  = {2'd0, scaled[11:10]};
            wire [3:0]  rem    = nibble - (fifth * 4'd5);
            assign coeff_ok = (nibble < 4'd15);
            assign coeff    = 32'sd2 - $signed({28'd0, rem});
        end else begin : g_eta4
            assign coeff_ok = (nibble < 4'd9);
            assign coeff    = 32'sd4 - $signed({28'd0, nibble});
        end
    endgenerate
endmodule

// 均匀采样的收集器：每周期吃一组 3 字节，攒够 256 个系数就置 done。
//
// 接口与 mlkem_rej_uniform 同形（start 触发 → 轮询 done → 组合读结果），
// 输入侧用 valid/ready 握手，因为字节流由上游 SHAKE 核按拍供给。
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
    reg [22:0] mem [0:255];
    reg        busy;

    assign rd_data  = mem[rd_addr];
    assign in_ready = busy;

    wire [22:0] cand;
    wire        cand_ok;
    mldsa_rej_uniform u_cand (
        .bytes_in(in_bytes), .cand(cand), .cand_ok(cand_ok));

    wire take = cand_ok && (count < 9'd256);

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
                mem[count[7:0]] <= cand;
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
