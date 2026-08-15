// ML-DSA 的 ExpandMask（FIPS 204 Alg 34）：给 ρ'' 和 nonce，出一条 mask 多项式 y
//
// ============================================================================
// 【这一层解决什么】
// ============================================================================
// 与 sampler.v 的两个采样器同形（挂共享 sha3_core，start→轮询 done→读系数），
// 但 ExpandMask **不是拒绝采样**：它把 SHAKE256(ρ''‖nonce) 挤出的定长字节流
// 直接按位解包成系数，没有「收不收」。
//   · 头部：ρ''（64 字节）‖ IntegerToBytes(nonce, 2)（小端两字节）；
//   · γ₁=2¹⁷ ⇒ 每系数 18 位，256 系数 = 4608 位 = 576 字节，正好整除；
//   · 逆变换 y = γ₁ − v（把无符号 18 位搬回 (−γ₁, γ₁]）。
//
// 位解包复用 mldsa_bitunpack（W=18）。576 字节喂完正好出 256 个系数，不多不少 ——
// 与两个拒绝采样器「攒够 256 立刻停」不同，这里靠字节数天然收口。
//
// ⚠️ 头部字节序、握手装下一字节、组合 ready、空敏感列表用连续赋值 —— 这些坑与
// sampler.v 完全同源，照抄它的写法（见那份文件的注释）。
`default_nettype none

// ⚠️ γ₁ 与它的位宽是**运行时输入**（运行时选 44/65/87 的一部分）。
//    只需要一个 cbits：γ₁ 恒等于 1 << (cbits − 1)
//    （cbits = 1 + bitlen(γ₁−1)，γ₁=2¹⁷→18，2¹⁹→20），
//    两个口会给"两者对不上"留下出错的余地。
module mldsa_expand_mask (
    input  wire        clk,
    input  wire        rst_n,

    input  wire [4:0]  cbits,         // 18（γ₁=2¹⁷）或 20（γ₁=2¹⁹）
    input  wire        start,          // 脉冲
    input  wire [511:0] seed,          // ρ''，64 字节；seed[7:0] 是第 0 字节
    input  wire [15:0] nonce,          // κ + r
    output reg         done,

    // ---- 挂共享 sha3_core ----
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

    // ---- 结果：done 之后按下标读 ----
    input  wire [7:0]  rd_addr,
    output wire signed [31:0] rd_data
);
    localparam [7:0] RATE = 8'd136, SUFFIX = 8'h1F;   // SHAKE256
    // γ₁ = 1 << (cbits − 1)，32 位有符号
    wire signed [31:0] G1 = 32'sd1 << (cbits - 5'd1);
    // 空敏感列表坑：连续赋值，不能用 always @(*)（见 sampler.v）
    assign sha_rate   = RATE;
    assign sha_suffix = SUFFIX;

    localparam [2:0] S_IDLE = 3'd0, S_ABS = 3'd1, S_GAP = 3'd2,
                     S_FLUSH = 3'd3, S_RUN = 3'd4, S_DONE = 3'd5;
    reg [2:0] st;
    reg [6:0] hdr_i;         // 头部字节：0..65

    // 头部：ρ'' 的 64 字节，然后 nonce 低字节、高字节
    wire [6:0] hdr_nxt_i = hdr_i + 7'd1;
    wire [7:0] hdr_byte = (hdr_i < 7'd64) ? seed[hdr_i*8 +: 8]
                        : (hdr_i == 7'd64) ? nonce[7:0]
                                           : nonce[15:8];
    // 握手那一拍装**下一个**字节（否则头部整个错一位，见 sampler.v）
    wire [7:0] hdr_byte_nxt = (hdr_nxt_i < 7'd64) ? seed[hdr_nxt_i*8 +: 8]
                            : (hdr_nxt_i == 7'd64) ? nonce[7:0]
                                                   : nonce[15:8];

    // 位解包器（W=CBITS）
    reg  [8:0] n;            // 已收下的系数个数
    wire        bu_ir, bu_ov;
    wire [19:0] bu_val;
    reg         bu_iv;
    wire        bu_or = bu_ov && (n < 9'd256);   // 有系数就抽
    // 解包器的位宽现在是运行时口；这一层的 CBITS 仍是编译期参数，直接喂常量。
    // （engine 落地后由 engine 用运行时 pset 驱动，这里只需把口接出去。）
    mldsa_bitunpack u_bu (
        .clk(clk), .rst_n(rst_n), .clr(st == S_IDLE), .w(cbits),
        .in_byte(sha_out_data), .in_valid(bu_iv), .in_ready(bu_ir),
        .out_val(bu_val), .out_valid(bu_ov), .out_ready(bu_or));

    // 逆变换 y = γ₁ − v，写进存储
    wire signed [31:0] coeff = G1 - $signed({12'd0, bu_val});

    // 只在「解包器要字节 且 还没满 且 不在抽系数」的拍次消费 SHAKE 字节。
    // bu_ir 与 bu_ov 互斥，所以「抽」优先、「喂」其次，两者不同拍。
    assign sha_out_ready = (st == S_RUN) && (n < 9'd256) && !bu_ov && bu_ir;
    always @(*) bu_iv = sha_out_ready && sha_out_valid;

    reg        we;
    reg [7:0]  waddr;
    reg signed [31:0] wdata;
    ram_dp #(.DW(32), .AW(8)) u_mem (
        .clk(clk),
        .a_we(we), .a_addr(waddr), .a_din(wdata), .a_dout(),
        .b_we(1'b0), .b_addr(rd_addr), .b_din(32'd0), .b_dout(rd_data));

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            st <= S_IDLE; done <= 1'b0; n <= 9'd0; hdr_i <= 7'd0;
            sha_start <= 1'b0; sha_in_valid <= 1'b0; sha_in_flush <= 1'b0;
            sha_in_data <= 8'd0; we <= 1'b0; waddr <= 8'd0; wdata <= 32'sd0;
        end else begin
            sha_start <= 1'b0;
            sha_in_valid <= 1'b0;
            sha_in_flush <= 1'b0;
            we <= 1'b0;
            done <= 1'b0;

            case (st)
            S_IDLE: if (start) begin
                n <= 9'd0; hdr_i <= 7'd0;
                sha_start <= 1'b1;
                st <= S_ABS;
            end

            S_ABS: begin
                sha_in_valid <= 1'b1;
                if (sha_in_valid && sha_in_ready) begin
                    if (hdr_i == 7'd65) begin
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

            S_GAP: st <= S_FLUSH;               // 空一拍让 in_valid 落下，flush 才被采样
            S_FLUSH: begin sha_in_flush <= 1'b1; st <= S_RUN; end

            S_RUN: begin
                // 抽一个系数（组合的 bu_or 已在这拍拉高）→ 存储、n++
                if (bu_ov && (n < 9'd256)) begin
                    we    <= 1'b1;
                    waddr <= n[7:0];
                    wdata <= coeff;
                    n     <= n + 9'd1;
                end
                if (n >= 9'd256) st <= S_DONE;   // 256 个到齐
            end

            S_DONE: begin done <= 1'b1; st <= S_IDLE; end
            default: st <= S_IDLE;
            endcase
        end
    end
endmodule

`default_nettype wire
