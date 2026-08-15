// ML-DSA KeyGen（FIPS 204 §5.1），增量搭建 —— 目前只做到第 ① 段 H
//
// ============================================================================
// 【为什么一段一段建，而不是一次写完】
// ============================================================================
// 上一版一口气把海绵三选一、所有存储、七个阶段塞进一个 500 行的模块，
// 结果是一个「能编译但没验过」的大块头，核心难点（换手时机、多级流水、
// 反压交汇）一条都没单独验证。这次改成增量：每加一段就有一个独立用例
// 把它对上黄金模型，绿了才加下一段。设计与全部七段的规划见
// docs/reference/mldsa-keygen-design.zh-CN.md。
//
// 目前实现：S_IDLE → H(ξ‖k‖ℓ) → ρ ‖ ρ' ‖ K，然后 done。
// ρ/ρ'/K 引成输出端口，测试台读出来对 hashlib.shake_256 逐字节。
//
// ============================================================================
// 【H 这一段的规格】
// ============================================================================
// H = SHAKE256(ξ‖IntegerToBytes(k,1)‖IntegerToBytes(ℓ,1)) 取 128 字节，
// 切成 ρ(32) ‖ ρ'(64) ‖ K(32)。ML-DSA-44 的 k=ℓ=4，所以尾巴是两个 0x04。
// 低地址字节先出（与 seed[i*8 +: 8] 的取法一致）。
`default_nettype none

module mldsa_keygen (
    input  wire        clk,
    input  wire        rst_n,

    input  wire        start,        // 脉冲
    input  wire [255:0] xi,          // 32 字节种子；xi[7:0] 是第 0 字节
    output reg         done,

    // ---- 共享的 sha3_core（KeyGen 全程只有一个海绵）----
    output reg         sha_start,
    output reg  [7:0]  sha_rate,
    output reg  [7:0]  sha_suffix,
    output reg         sha_in_valid,
    output reg  [7:0]  sha_in_data,
    output reg         sha_in_flush,
    input  wire        sha_in_ready,
    input  wire        sha_out_valid,
    output reg         sha_out_ready,
    input  wire [7:0]  sha_out_data,

    // ---- 派生量（done 之后有效）----
    output reg [255:0] rho,
    output reg [511:0] rho_prime,
    output reg [255:0] key_out,

    // ---- 调试读口：done 之后读 s₁/s₂ 的系数，供逐段验证 ----
    // sel[3] 选 s₁(0)/s₂(1)，sel[1:0] 选第几条（ℓ=k=4，用不到 sel[2]），
    // idx 选系数。
    input  wire [3:0]  dbg_sel,
    input  wire [7:0]  dbg_idx,
    output wire signed [31:0] dbg_coef
);
    // k、ℓ 用 8 位常量而不是 integer：尾字节要直接取它们的低 8 位
    // （FIPS 204 的 H 输入是 ξ‖IntegerToBytes(k,1)‖IntegerToBytes(ℓ,1)）。
    localparam [7:0] K = 8'd4, L = 8'd4;
    localparam [7:0] RATE256 = 8'd136, SUF = 8'h1F;   // SHAKE256
    localparam integer ETA = 2;

    localparam [3:0]
        S_IDLE  = 4'd0, S_H_ABS = 4'd1, S_H_GAP = 4'd2,
        S_H_FLU = 4'd3, S_H_SQ  = 4'd4,
        S_S_GEN = 4'd5, S_S_WAIT = 4'd6, S_S_MOVE = 4'd7,
        S_FIN   = 4'd8;

    reg [3:0] st;
    reg [8:0] cnt;        // 吸收/挤压计数

    // H 的输入：ξ(32) ‖ k ‖ ℓ，一共 34 字节
    wire [7:0] h_byte = (cnt < 9'd32) ? xi[cnt*8 +: 8]
                      : (cnt == 9'd32) ? K[7:0] : L[7:0];
    // 握手那一拍要装的是**下一个**字节（见 sampler.v 里同一个坑）
    wire [8:0] cnxt = cnt + 9'd1;
    wire [7:0] h_byte_nxt = (cnxt < 9'd32) ? xi[cnxt*8 +: 8]
                          : (cnxt == 9'd32) ? K[7:0] : L[7:0];

    // ================= 海绵归属（FSM ↔ η 采样器）=================
    // 只在换手方空闲时切（见设计文档）。均匀采样器留到第 ④ 段再接。
    localparam OWN_FSM = 1'b0, OWN_ETA = 1'b1;
    reg owner;

    // ---- η 采样器（ExpandS）----
    reg         et_start;
    reg  [15:0] et_nonce;
    wire        et_done;
    reg  [7:0]  et_rd_addr;
    wire signed [31:0] et_rd_data;
    wire        et_ss, et_siv, et_sif, et_sor;
    wire [7:0]  et_sr, et_su, et_sid;

    mldsa_poly_eta #(.ETA(ETA)) u_eta (
        .clk(clk), .rst_n(rst_n),
        .start(et_start), .seed(rho_prime), .nonce(et_nonce), .done(et_done),
        .sha_start(et_ss), .sha_rate(et_sr), .sha_suffix(et_su),
        .sha_in_valid(et_siv), .sha_in_data(et_sid), .sha_in_flush(et_sif),
        .sha_in_ready(sha_in_ready && (owner == OWN_ETA)),
        .sha_out_valid(sha_out_valid && (owner == OWN_ETA)),
        .sha_out_ready(et_sor), .sha_out_data(sha_out_data),
        .rd_addr(et_rd_addr), .rd_data(et_rd_data), .count());

    // FSM 自己驱动海绵时用的那组线
    reg        fsm_ss, fsm_siv, fsm_sif, fsm_sor;
    reg [7:0]  fsm_sr, fsm_su, fsm_sid;

    // ---- 系数存储：s₁(ℓ 条) 与 s₂(k 条)，各 1024×32 ----
    reg         s1_we;  reg [9:0] s1_waddr; reg signed [31:0] s1_din;
    wire signed [31:0] s1_dout;
    reg         s2_we;  reg [9:0] s2_waddr; reg signed [31:0] s2_din;
    wire signed [31:0] s2_dout;
    reg  [9:0]  s1_raddr, s2_raddr;
    ram_dp #(.DW(32), .AW(10)) u_s1 (
        .clk(clk), .a_we(s1_we), .a_addr(s1_waddr), .a_din(s1_din), .a_dout(),
        .b_we(1'b0), .b_addr(s1_raddr), .b_din(32'd0), .b_dout(s1_dout));
    ram_dp #(.DW(32), .AW(10)) u_s2 (
        .clk(clk), .a_we(s2_we), .a_addr(s2_waddr), .a_din(s2_din), .a_dout(),
        .b_we(1'b0), .b_addr(s2_raddr), .b_din(32'd0), .b_dout(s2_dout));

    // 调试读口挂 b 口（done 后才用，与写不重叠）
    assign dbg_coef = dbg_sel[3] ? s2_dout : s1_dout;

    // η 打包器留到 sk 组装段再接：第 ② 段只做「采样 + 存储」，
    // 而 η 打包（对 polyeta_pack）已在 test_mldsa_pack 里独立验过，
    // 这里空转它只会多一堆未接的输出。
    // 海绵接口二选一
    always @(*) begin
        if (owner == OWN_ETA) begin
            sha_start = et_ss; sha_rate = et_sr; sha_suffix = et_su;
            sha_in_valid = et_siv; sha_in_data = et_sid; sha_in_flush = et_sif;
            sha_out_ready = et_sor;
        end else begin
            sha_start = fsm_ss; sha_rate = fsm_sr; sha_suffix = fsm_su;
            sha_in_valid = fsm_siv; sha_in_data = fsm_sid; sha_in_flush = fsm_sif;
            sha_out_ready = fsm_sor;
        end
    end

    reg       ph;             // 同步读的两拍相位
    reg [2:0] vj;             // 第几条多项式
    reg       s2phase;        // 在做 s₂ 而不是 s₁

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            st <= S_IDLE; done <= 1'b0; cnt <= 9'd0;
            owner <= OWN_FSM; vj <= 3'd0; ph <= 1'b0; s2phase <= 1'b0;
            et_start <= 1'b0; et_nonce <= 16'd0;
            fsm_ss <= 1'b0; fsm_siv <= 1'b0; fsm_sif <= 1'b0; fsm_sor <= 1'b0;
            fsm_sr <= RATE256; fsm_su <= SUF; fsm_sid <= 8'd0;
            rho <= 256'd0; rho_prime <= 512'd0; key_out <= 256'd0;
        end else begin
            fsm_ss  <= 1'b0;
            fsm_siv <= 1'b0;
            fsm_sif <= 1'b0;
            et_start <= 1'b0;
            done    <= 1'b0;

            case (st)
            S_IDLE: if (start) begin
                cnt <= 9'd0; owner <= OWN_FSM;
                fsm_sr <= RATE256; fsm_su <= SUF;
                fsm_ss <= 1'b1;
                fsm_sid <= xi[7:0];
                st <= S_H_ABS;
            end

            // ---------- ① H(ξ‖k‖ℓ) → ρ ‖ ρ' ‖ K ----------
            S_H_ABS: begin
                fsm_siv <= 1'b1;
                if (fsm_siv && sha_in_ready) begin
                    if (cnt == 9'd33) begin
                        fsm_siv <= 1'b0;
                        st <= S_H_GAP;
                    end else begin
                        cnt <= cnxt;
                        fsm_sid <= h_byte_nxt;
                    end
                end else begin
                    fsm_sid <= h_byte;
                end
            end
            S_H_GAP: st <= S_H_FLU;
            S_H_FLU: begin fsm_sif <= 1'b1; cnt <= 9'd0; st <= S_H_SQ; end

            S_H_SQ: begin
                fsm_sor <= 1'b1;
                if (sha_out_valid) begin
                    if (cnt < 9'd32)       rho       <= {sha_out_data, rho[255:8]};
                    else if (cnt < 9'd96)  rho_prime <= {sha_out_data, rho_prime[511:8]};
                    else                   key_out   <= {sha_out_data, key_out[255:8]};
                    if (cnt == 9'd127) begin
                        fsm_sor <= 1'b0;
                        cnt <= 9'd0; vj <= 3'd0; s2phase <= 1'b0;
                        owner <= OWN_ETA;      // 换手给 η 采样器
                        st <= S_S_GEN;
                    end else begin
                        cnt <= cnt + 9'd1;
                    end
                end
            end

            // ---------- ② ExpandS：先 ℓ 条 s₁，再 k 条 s₂ ----------
            // nonce：s₁ 用 j，s₂ 用 ℓ+j（ℓ=4）
            S_S_GEN: begin
                et_nonce <= s2phase ? (16'd4 + {13'd0, vj}) : {13'd0, vj};
                et_start <= 1'b1;
                st <= S_S_WAIT;
            end
            S_S_WAIT: if (et_done) begin
                cnt <= 9'd0; ph <= 1'b0;
                st <= S_S_MOVE;
            end

            // 采样器读口同步，两拍一个系数：ph=0 摆地址，ph=1 用数据。
            // 用数据这一拍：写进 s1/s2 存储 + 喂 η 打包器（有反压）。
            // 无打包器反压了（打包留到 sk 组装段），所以两拍一个系数直接推进。
            S_S_MOVE: begin
                if (!ph) begin
                    ph <= 1'b1;
                end else begin
                    if (cnt == 9'd255) begin
                        cnt <= 9'd0; ph <= 1'b0;
                        if (!s2phase && vj == 3'd3) begin
                            s2phase <= 1'b1; vj <= 3'd0; st <= S_S_GEN;
                        end else if (s2phase && vj == 3'd3) begin
                            vj <= 3'd0; owner <= OWN_FSM;
                            st <= S_FIN;       // 第 ② 段到此为止
                        end else begin
                            vj <= vj + 3'd1; st <= S_S_GEN;
                        end
                    end else begin
                        cnt <= cnt + 9'd1; ph <= 1'b0;
                    end
                end
            end

            S_FIN: begin done <= 1'b1; st <= S_IDLE; end
            default: st <= S_IDLE;
            endcase
        end
    end

    // ================= 端口归属（组合）=================
    always @(*) begin
        s1_we = 1'b0; s1_waddr = 10'd0; s1_din = 32'd0;
        s2_we = 1'b0; s2_waddr = 10'd0; s2_din = 32'd0;
        s1_raddr = {dbg_sel[1:0], dbg_idx};   // 默认给调试口
        s2_raddr = {dbg_sel[1:0], dbg_idx};
        et_rd_addr = cnt[7:0];
        if (st == S_S_MOVE && ph) begin
            if (!s2phase) begin
                s1_we = 1'b1; s1_waddr = {vj[1:0], cnt[7:0]}; s1_din = et_rd_data;
            end else begin
                s2_we = 1'b1; s2_waddr = {vj[1:0], cnt[7:0]}; s2_din = et_rd_data;
            end
        end
    end
endmodule
