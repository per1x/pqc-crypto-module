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
    output reg [255:0] key_out
);
    // k、ℓ 用 8 位常量而不是 integer：尾字节要直接取它们的低 8 位
    // （FIPS 204 的 H 输入是 ξ‖IntegerToBytes(k,1)‖IntegerToBytes(ℓ,1)）。
    localparam [7:0] K = 8'd4, L = 8'd4;
    localparam [7:0] RATE256 = 8'd136, SUF = 8'h1F;   // SHAKE256

    localparam [2:0]
        S_IDLE  = 3'd0, S_H_ABS = 3'd1, S_H_GAP = 3'd2,
        S_H_FLU = 3'd3, S_H_SQ  = 3'd4, S_FIN   = 3'd5;

    reg [2:0] st;
    reg [8:0] cnt;        // 吸收/挤压计数

    // H 的输入：ξ(32) ‖ k ‖ ℓ，一共 34 字节
    wire [7:0] h_byte = (cnt < 9'd32) ? xi[cnt*8 +: 8]
                      : (cnt == 9'd32) ? K[7:0] : L[7:0];
    // 握手那一拍要装的是**下一个**字节（见 sampler.v 里同一个坑）
    wire [8:0] cnxt = cnt + 9'd1;
    wire [7:0] h_byte_nxt = (cnxt < 9'd32) ? xi[cnxt*8 +: 8]
                          : (cnxt == 9'd32) ? K[7:0] : L[7:0];

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            st <= S_IDLE; done <= 1'b0; cnt <= 9'd0;
            sha_start <= 1'b0; sha_in_valid <= 1'b0; sha_in_flush <= 1'b0;
            sha_out_ready <= 1'b0; sha_in_data <= 8'd0;
            sha_rate <= RATE256; sha_suffix <= SUF;
            rho <= 256'd0; rho_prime <= 512'd0; key_out <= 256'd0;
        end else begin
            sha_start    <= 1'b0;
            sha_in_valid <= 1'b0;
            sha_in_flush <= 1'b0;
            done         <= 1'b0;

            case (st)
            S_IDLE: if (start) begin
                cnt <= 9'd0;
                sha_rate <= RATE256; sha_suffix <= SUF;
                sha_start <= 1'b1;
                sha_in_data <= xi[7:0];
                st <= S_H_ABS;
            end

            S_H_ABS: begin
                sha_in_valid <= 1'b1;
                if (sha_in_valid && sha_in_ready) begin
                    if (cnt == 9'd33) begin
                        sha_in_valid <= 1'b0;
                        st <= S_H_GAP;
                    end else begin
                        cnt <= cnxt;
                        sha_in_data <= h_byte_nxt;
                    end
                end else begin
                    sha_in_data <= h_byte;   // 还没握上，保持当前字节
                end
            end

            // 空一拍让 in_valid 落下来，flush 才被采样（见设计文档）
            S_H_GAP: st <= S_H_FLU;
            S_H_FLU: begin sha_in_flush <= 1'b1; cnt <= 9'd0; st <= S_H_SQ; end

            S_H_SQ: begin
                sha_out_ready <= 1'b1;
                if (sha_out_valid) begin
                    // 低地址字节先出：从高位塞、整体右移
                    if (cnt < 9'd32)       rho       <= {sha_out_data, rho[255:8]};
                    else if (cnt < 9'd96)  rho_prime <= {sha_out_data, rho_prime[511:8]};
                    else                   key_out   <= {sha_out_data, key_out[255:8]};
                    if (cnt == 9'd127) begin
                        sha_out_ready <= 1'b0;
                        st <= S_FIN;
                    end else begin
                        cnt <= cnt + 9'd1;
                    end
                end
            end

            S_FIN: begin done <= 1'b1; st <= S_IDLE; end
            default: st <= S_IDLE;
            endcase
        end
    end
endmodule
