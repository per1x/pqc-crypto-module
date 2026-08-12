// trng_cond —— 熵调理器：Keccak 海绵
//
// 【为什么必须有调理，以及为什么用 Keccak】
// 环振采出来的原始比特是有偏、有相关的：可能 55% 的 1、相邻比特之间有残余
// 相关。直接拿去当密钥就是错的。调理器的作用是把 N 个低熵比特压成 M 个
// 接近满熵的比特（N > M），代价是吞吐下降。
//
// SP 800-90B §3.1.5.1.2 给了一份"vetted conditioning components"清单：
// HMAC、CMAC、CBC-MAC、Hash_df、以及**哈希函数本身**（含 SHA-3 家族）。
// 用清单里的构件，输出熵可以直接按 min(输出长度, 输入熵) 计；用自己发明的
// 白化（von Neumann、LFSR 打散之类）则要另行论证。
//
// 选 Keccak 还有个工程上的理由：`keccak_f1600` 这个核仓库里已经有了、已经过
// cocotb 对拍。TRNG 复用它，等于零增量面积拿到一个 vetted 构件；也顺带证明
// 那个核是可复用的 IP，而不是只能给 pqc_accel_axi 用的一次性件。
//
// 【参数取值与熵账】
//   rate     = 17 lane = 1088 bit（SHA3-256 的 rate）
//   capacity = 512 bit → 256 位安全强度
//   每轮吸收 1088 个原始比特，置换一次，挤出 4 lane = 256 bit
//
// 按 H = 0.5 bit/样本 的假设，一轮进来 1088 × 0.5 = 544 bit 熵，挤出 256 bit。
// 544 > 256，输出可按满熵计。**这笔账的前提是 H ≥ 0.5，而 H 尚未在硅上实测**
// （见 ring_osc.v 文件头）。实测若低于 0.235 bit/样本，就要改比例 —— 把
// ABSORB_BLOCKS 调大，每挤出一次前多吸收几个 rate 块。参数留在这里就是为此。
//
// 【海绵状态从不重置】
// 吸收 → 置换 → 挤出 → 继续吸收，用的是同一份状态，中间没有重新初始化。
// 这是 duplex 模式：两次挤出之间必定隔着一次完整置换，所以前向安全；同时
// 早先积累的熵不会被丢弃，等于一个持续再播种的熵池 —— 真密码机里的熵池
// 就是这么工作的。只有 zeroize 才把状态清零（由上层拉 rst_n 实现）。
//
// 【背压：吸收期丢比特是安全的】
// 置换和挤出期间（约 24 + 10 个时钟）进来的原始比特会被丢弃。丢比特损失的是
// 熵率而不是安全性 —— 少收总比收进不该收的强。按 DECIM=8 算，每 8704 个
// 时钟的吸收期只对应丢掉 4 个样本左右，可以忽略。bit_ready 接出来只为观测。
`default_nettype none

module trng_cond #(
    parameter integer RATE_LANES    = 17,  // 每块吸收的 lane 数（1088 bit）
    parameter integer ABSORB_BLOCKS = 1,   // 每次挤出前吸收几个 rate 块
    parameter integer OUT_LANES     = 4    // 每次挤出的 lane 数（256 bit）
) (
    input  wire        clk,
    input  wire        rst_n,              // 拉低即清空海绵状态（zeroize）

    // 原始噪声比特入口
    input  wire        bit_valid,
    input  wire        bit_in,
    output wire        bit_ready,          // 观测用；为低时进来的比特被丢弃

    // 调理后的 32 位字出口
    output wire        word_valid,
    output wire [31:0] word_out,
    input  wire        word_ready,

    // 观测口
    output reg  [31:0] blocks_absorbed     // 已完成的 rate 块数
);

    localparam [3:0] S_ABSORB    = 4'd0,
                     S_XOR       = 4'd1,
                     S_PERM_KICK = 4'd2,
                     S_PERM_WAIT = 4'd3,
                     S_SQUEEZE   = 4'd4;

    reg  [3:0]  state;
    reg  [63:0] lane_sr;                   // 正在拼装的 lane
    reg  [5:0]  bit_cnt;                   // 0..63
    reg  [4:0]  lane_idx;                  // 本块吸收到第几个 lane
    reg  [15:0] blk_cnt;                   // 本轮已吸收几个 rate 块
    reg  [4:0]  sq_lane;                   // 挤出到第几个 lane
    reg         sq_half;                   // 0=低 32 位，1=高 32 位

    // ---- Keccak 核 ----
    reg          kec_start, kec_wr_en;
    reg  [4:0]   kec_wr_addr;
    reg  [63:0]  kec_wr_data;
    wire [63:0]  kec_rd_data;
    wire         kec_done;

    // 读地址是组合的：吸收时读要异或的那个 lane，挤出时读要输出的那个 lane。
    // keccak_f1600 的读口本来就是组合读，这样两处都不需要额外的等地址周期。
    wire [4:0]   kec_rd_addr = (state == S_SQUEEZE) ? sq_lane : lane_idx;

    keccak_f1600 u_keccak (
        .clk(clk), .rst_n(rst_n),
        .start(kec_start), .done(kec_done),
        .wr_en(kec_wr_en), .wr_addr(kec_wr_addr), .wr_data(kec_wr_data),
        .rd_addr(kec_rd_addr), .rd_data(kec_rd_data));

    // ---- 出口握手 ----
    // 组合驱动：挤出状态下始终有效，word_ready 拉高即完成一拍。
    assign word_valid = (state == S_SQUEEZE);
    assign word_out   = sq_half ? kec_rd_data[63:32] : kec_rd_data[31:0];
    assign bit_ready  = (state == S_ABSORB);

    wire word_hs = word_valid && word_ready;
    wire last_sq = (sq_lane == OUT_LANES[4:0] - 5'd1) && sq_half;

    // 核的 done 是电平语义：上一次置换的 done 会一直保持到下次 start。
    // kick_busy 高的那一拍正是核看到 start 的那一拍，此时 done 还没清，必须跳过。
    // 这一条与 pqc_accel_axi.v 的 S_WAIT 是同一个坑，同一个解法。
    wire kick_busy = kec_start;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state           <= S_ABSORB;
            lane_sr         <= 64'd0;
            bit_cnt         <= 6'd0;
            lane_idx        <= 5'd0;
            blk_cnt         <= 16'd0;
            sq_lane         <= 5'd0;
            sq_half         <= 1'b0;
            kec_start       <= 1'b0;
            kec_wr_en       <= 1'b0;
            kec_wr_addr     <= 5'd0;
            kec_wr_data     <= 64'd0;
            blocks_absorbed <= 32'd0;
        end else begin
            kec_start <= 1'b0;
            kec_wr_en <= 1'b0;

            case (state)
            S_ABSORB: begin
                if (bit_valid) begin
                    // LSB 优先移入：第一个比特最终落在 lane 的 bit 0
                    lane_sr <= {bit_in, lane_sr[63:1]};
                    if (bit_cnt == 6'd63) begin
                        bit_cnt <= 6'd0;
                        state   <= S_XOR;
                    end else begin
                        bit_cnt <= bit_cnt + 6'd1;
                    end
                end
            end

            S_XOR: begin
                // 读-改-写：核只提供覆盖式写口，海绵要的是异或注入
                kec_wr_en   <= 1'b1;
                kec_wr_addr <= lane_idx;
                kec_wr_data <= kec_rd_data ^ lane_sr;
                lane_sr     <= 64'd0;

                if (lane_idx == RATE_LANES[4:0] - 5'd1) begin
                    lane_idx        <= 5'd0;
                    blocks_absorbed <= blocks_absorbed + 32'd1;
                    // 吸收满 ABSORB_BLOCKS 个 rate 块才置换并挤出。
                    // 中间那些块也要各自置换一次，否则超出 rate 的部分会被
                    // 后续块直接覆盖掉，多吸收的熵白丢。
                    if (blk_cnt == ABSORB_BLOCKS[15:0] - 16'd1) begin
                        blk_cnt <= 16'd0;
                    end else begin
                        blk_cnt <= blk_cnt + 16'd1;
                    end
                    state <= S_PERM_KICK;
                end else begin
                    lane_idx <= lane_idx + 5'd1;
                    state    <= S_ABSORB;
                end
            end

            S_PERM_KICK: begin
                // 这一拍 kec_wr_en 仍为高，最后一个 lane 的写与 start 同拍生效：
                // 核在 !busy 分支里先落 A[wr_addr]（非阻塞），同拍置 busy，
                // 下一拍才开始算第 0 轮，读到的已是写入后的状态。
                kec_start <= 1'b1;
                state     <= S_PERM_WAIT;
            end

            S_PERM_WAIT: begin
                if (!kick_busy && kec_done) begin
                    sq_lane <= 5'd0;
                    sq_half <= 1'b0;
                    // 只有整轮吸收够了才挤出，否则接着吸收下一个 rate 块
                    state   <= (blk_cnt == 16'd0) ? S_SQUEEZE : S_ABSORB;
                end
            end

            S_SQUEEZE: begin
                if (word_hs) begin
                    if (last_sq) begin
                        state <= S_ABSORB;
                    end else if (sq_half) begin
                        sq_half <= 1'b0;
                        sq_lane <= sq_lane + 5'd1;
                    end else begin
                        sq_half <= 1'b1;
                    end
                end
            end

            default: state <= S_ABSORB;
            endcase
        end
    end

endmodule

`default_nettype wire
