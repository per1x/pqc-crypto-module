// ML-DSA 的 SampleInBall（FIPS 204 Alg 29）：给 c̃，出稀疏多项式 c（τ 个 ±1，其余 0）
//
// ============================================================================
// 【这一层解决什么】
// ============================================================================
// c = SampleInBall(c̃)：SHAKE256(c̃) 的字节流里，前 8 字节当 64 位符号串 signs，
// 之后按 Fisher–Yates 式在 [0, i] 上取落点 b（b>i 就丢弃再取），把 c[i] 挪到 c[b]、
// c[b] 置 ±1（符号取自 signs 的最低位，用完右移）。i 从 N−τ 到 N−1，共放 τ 个 ±1。
//
// c 的系数只有 {−1,0,1}，用 256×2 位有符号寄存器阵列承载（512 FF，很小），
// 好处是「读 c[b]、写 c[i]、写 c[b]」能在一拍内用非阻塞赋值完成交换：
//   c[i] <= c[b];  c[b] <= sign;   b==i 时后者胜出，结果 c[i]=sign（与参考实现一致）。
//
// ⚠️ 「b>i 要丢弃继续取」不能省 —— SHAKE 是流，取到超界的字节就再取一个，直到 ≤i。
// 头部字节序 / 握手 / 组合 ready / 空敏感列表用连续赋值等坑与 sampler.v 同源。
`default_nettype none

// ⚠️ τ 与 c̃ 长度是**运行时输入**（运行时选 44/65/87 的一部分）。
//    seed 端口因此固定成**最宽的 512 位**（c̃ 最长 64 字节），只读前 ctb 字节；
//    原来它是 CTB*8 位，宽度随参数变，运行时化之后不能再那样。
module mldsa_sample_in_ball (
    input  wire        clk,
    input  wire        rst_n,

    input  wire [6:0]  tau,               // 39 / 49 / 60
    input  wire [6:0]  ctb,               // c̃ 字节数 32 / 48 / 64（λ/4）
    input  wire        start,
    input  wire [511:0] seed,             // c̃，低 ctb 字节有效；seed[7:0] 是第 0 字节
    output reg         done,

    output reg         sha_start,
    output wire [7:0]  sha_rate,
    output wire [7:0]  sha_suffix,
    output reg         sha_in_valid,
    output reg  [7:0]  sha_in_data,
    output reg         sha_in_flush,
    input  wire        sha_in_ready,
    input  wire        sha_out_valid,
    output wire        sha_out_ready,
    input  wire [7:0]  sha_out_data,

    input  wire [7:0]  rd_addr,
    output wire signed [31:0] rd_data
);
    localparam [7:0] RATE = 8'd136, SUFFIX = 8'h1F;   // SHAKE256
    // N − τ（τ=39 时 217）。在 9 位里算再截，免得 256 在 8 位里变成 0。
    wire [8:0] start_i9 = 9'd256 - {2'b0, tau};
    wire [7:0] START_I  = start_i9[7:0];
    assign sha_rate   = RATE;
    assign sha_suffix = SUFFIX;

    localparam [2:0] S_IDLE = 3'd0, S_ABS = 3'd1, S_GAP = 3'd2, S_FLUSH = 3'd3,
                     S_SIGN = 3'd4, S_BYTE = 3'd5, S_PLACE = 3'd6, S_DONE = 3'd7;
    reg [2:0] st;
    reg [6:0] hdr_i;         // 0..CTB-1（CTB 最大 64）
    reg [3:0] scnt;          // signs 的 8 字节计数 0..7
    reg [63:0] signs;
    reg [7:0]  ii;           // 当前落点 i（START_I..255）
    reg [7:0]  bb;           // 当前取到的字节 b

    reg signed [1:0] cc [0:255];
    integer kk;

    // 头部：c̃ 的 32 字节
    wire [6:0] hdr_nxt_i = hdr_i + 7'd1;
    // ⚠️ 末字节要钳位：hdr_nxt_i 在最后一拍会走到 CTB，直接切片会越界读到 X
    wire [6:0] hdr_nxt_c = (hdr_nxt_i >= ctb) ? (ctb - 7'd1) : hdr_nxt_i;
    wire [7:0] hdr_byte     = seed[hdr_i*8 +: 8];
    wire [7:0] hdr_byte_nxt = seed[hdr_nxt_c*8 +: 8];

    // 只在需要字节的挤压态抽 SHAKE：读 signs、或在 S_BYTE 里取落点。
    assign sha_out_ready = (st == S_SIGN) || (st == S_BYTE);

    assign rd_data = {{30{cc[rd_addr][1]}}, cc[rd_addr]};

    // 本拍要放的符号：signs 最低位为 1 → −1，否则 +1
    wire signed [1:0] cur_sign = signs[0] ? -2'sd1 : 2'sd1;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            st <= S_IDLE; done <= 1'b0; hdr_i <= 7'd0; scnt <= 4'd0;
            signs <= 64'd0; ii <= START_I; bb <= 8'd0;
            sha_start <= 1'b0; sha_in_valid <= 1'b0; sha_in_flush <= 1'b0;
            sha_in_data <= 8'd0;
        end else begin
            sha_start <= 1'b0;
            sha_in_valid <= 1'b0;
            sha_in_flush <= 1'b0;
            done <= 1'b0;

            case (st)
            S_IDLE: if (start) begin
                hdr_i <= 7'd0; scnt <= 4'd0; ii <= START_I;
                for (kk = 0; kk < 256; kk = kk + 1) cc[kk] <= 2'sd0;
                sha_start <= 1'b1;
                st <= S_ABS;
            end

            S_ABS: begin
                sha_in_valid <= 1'b1;
                if (sha_in_valid && sha_in_ready) begin
                    if (hdr_i == ctb - 7'd1) begin
                        sha_in_valid <= 1'b0;
                        st <= S_GAP;
                    end else begin
                        hdr_i       <= hdr_nxt_i;
                        sha_in_data <= hdr_byte_nxt;
                    end
                end else begin
                    sha_in_data <= hdr_byte;
                end
            end
            S_GAP: st <= S_FLUSH;
            S_FLUSH: begin sha_in_flush <= 1'b1; st <= S_SIGN; end

            // 前 8 字节 → signs（小端 64 位）
            S_SIGN: if (sha_out_valid) begin
                signs <= {sha_out_data, signs[63:8]};
                if (scnt == 4'd7) st <= S_BYTE;
                else scnt <= scnt + 4'd1;
            end

            // 取落点：b>i 就丢弃再取；b≤i 落进 S_PLACE
            S_BYTE: if (sha_out_valid) begin
                bb <= sha_out_data;
                if (sha_out_data <= ii) st <= S_PLACE;
            end

            // 交换：c[i]=c[b]；c[b]=±1；signs 右移；i++
            S_PLACE: begin
                cc[ii] <= cc[bb];
                cc[bb] <= cur_sign;
                signs  <= {1'b0, signs[63:1]};
                if (ii == 8'd255) st <= S_DONE;
                else begin ii <= ii + 8'd1; st <= S_BYTE; end
            end

            S_DONE: begin done <= 1'b1; st <= S_IDLE; end
            default: st <= S_IDLE;
            endcase
        end
    end
endmodule

`default_nettype wire
