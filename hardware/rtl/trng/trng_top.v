// trng_top —— 完整 TRNG：噪声源 + 健康检测 + 调理 + 输出缓冲
//
// 把 ring_osc / trng_source / trng_health / trng_cond / sync_fifo 串成一条
// 符合 SP 800-90B 结构的熵源。链路：
//
//   环振阵列 ──采样/抽取──> 原始比特 ──┬──> 连续健康检测（RCT + APT）
//                                      └──> Keccak 海绵调理 ──> FIFO ──> 软件
//
// 【三条策略性的取舍，都在这一层】
//
// 1. 启动健康检测（§4.3）：上电后先连续吃 STARTUP_SAMPLES 个样本、全部通过
//    连续检测，才允许调理器工作。这段时间的样本**全部丢弃**，一个都不进海绵。
//    标准要求的是 1024 个样本，这里默认就是 1024。
//
// 2. 告警之后怎么办（§4.4 只要求"停止输出并上报"，动作由使用方定）：
//    本设计选最保守的一种 —— 告警立刻拉低 ready、清空并擦除 FIFO、
//    **把调理器连同海绵状态一起复位**、启动检测重新来过。
//    为什么连熵池一起清：告警意味着噪声源可能已经失效一段时间了，池子里
//    可能已经混进了低熵输入，留着它比丢掉它风险大。代价只是重新暖机。
//
// 3. zeroize：清空 FIFO（逐地址覆零，不是只挪指针）、复位海绵状态、
//    重跑启动检测。这是密码边界的 zeroize-on-tamper 挂钩点 —— 上层把
//    篡改检测信号接到这个口上即可。
//
// 【复位的写法】
// 派生复位（cond_rst_n）先寄存一拍再驱动，不把 zeroize / alarm 组合进
// 异步复位网络。组合出来的异步复位有毛刺风险，Vivado 也会就此报 DRC。
// 这与 pqc_accel_axi.v 里 core_rst_n 的处理是同一条规矩。
`default_nettype none

module trng_top #(
    parameter integer NUM_RO          = 8,     // 环振条数
    parameter integer RO_STAGES_0     = 13,    // 第 0 条的级数，之后每条 +2
    parameter integer DECIM           = 8,     // 采样抽取比
    parameter integer RCT_CUTOFF      = 47,    // 实测 H=0.871234、α=2⁻⁴⁰，见 trng_health.v
    parameter integer APT_WINDOW      = 1024,
    parameter integer APT_CUTOFF      = 672,   // 同上；旧值 793 在实测熵下永不触发
    parameter integer STARTUP_SAMPLES = 1024,  // SP 800-90B §4.3
    parameter integer RATE_LANES      = 17,    // 调理器 rate（1088 bit）
    parameter integer ABSORB_BLOCKS   = 1,
    parameter integer OUT_LANES       = 4,     // 每次挤出 256 bit
    parameter integer FIFO_DEPTH      = 16,
    // ============================================================================
    // 【RAW_TAP：把**调理前**的原始噪声比特接到一个软件可读的口上】
    // ============================================================================
    // 为什么必须有：SP 800-90B 的最小熵评估要的是**噪声源的原始数字化样本**，
    // 不是调理器（SHA-3）的输出 —— 调理器的输出无论熵多低看着都像随机数，
    // 拿它跑 EntropyAssessment 得到的数字是**无意义的**，而且会得到一个
    // 非常好看的数字，正好骗过想少做一步的人。
    //
    // 抽头点取 src_valid/src_bit，也就是**健康检测（RCT/APT）吃的同一条流**。
    // 检测的对象、评估的对象、被使用的对象必须是同一个，否则三者都没有意义。
    //
    // ⚠️ **这是表征用的口，不是产品形态。** 把噪声源的原始比特摆在总线上，
    //    等于把熵源的内部状态直接给了读它的人。所以：
    //      · 默认 RAW_TAP=0，整条通路连同寄存器一起不存在（不是"读了返回 0"）；
    //      · 打开它的构建只用于跑 SP 800-90B 取数，取完就换回 0；
    //      · 即使打开，它仍在 AXI 防火墙之后，生产形态下只有安全世界够得到。
    parameter integer RAW_TAP         = 0
) (
    input  wire        clk,
    input  wire        rst_n,

    input  wire        enable,       // 低电平：环振停振（省电、不辐射）
    input  wire        zeroize,      // 脉冲：擦除所有中间态并重跑启动检测
    input  wire        clear_alarm,  // 脉冲：清告警并重跑启动检测

    // 随机字出口
    input  wire        rd_en,
    output wire [31:0] rd_data,
    output wire        rd_valid,

    // 状态
    output wire        ready,          // 启动检测已过、无告警、未在 zeroize
    output wire        alarm,
    output wire        rct_alarm,
    output wire        apt_alarm,
    output reg         startup_done,
    output wire        fifo_wiping,

    // 观测口（软件读健康状态、做产线自测用）
    output wire [15:0] rct_run,
    output wire [15:0] apt_count,
    output wire [15:0] apt_index,
    output reg  [31:0] startup_count,
    output wire [31:0] blocks_absorbed,
    output reg  [31:0] words_out,

    // ---- 原始噪声抽头（RAW_TAP=1 时才有东西）----
    input  wire        raw_rd_en,
    output wire [31:0] raw_data,
    output wire        raw_valid
);

    // ---- zeroize 展宽 ----
    // 一个时钟的脉冲不足以让下游的异步复位可靠生效，展成若干拍。
    reg [2:0] zero_hold;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            zero_hold <= 3'd0;
        end else if (zeroize) begin
            zero_hold <= 3'd7;
        end else if (zero_hold != 3'd0) begin
            zero_hold <= zero_hold - 3'd1;
        end
    end
    wire zeroize_active = (zero_hold != 3'd0);

    // ---- 噪声源 ----
    wire src_valid, src_bit;

    trng_source #(
        .NUM_RO(NUM_RO), .RO_STAGES_0(RO_STAGES_0), .DECIM(DECIM)
    ) u_src (
        .clk(clk), .rst_n(rst_n),
        .enable(enable && !zeroize_active),
        .sample_valid(src_valid), .sample(src_bit));

    // ---- 原始噪声抽头 ----
    // 把 src_bit 每 32 个攒成一个字，压进一个小 FIFO 供软件取。
    // 满了就**丢新的**，不做背压 —— 抽头绝不能拖慢噪声源本身，那会改变
    // 被评估的那条流的统计性质，等于评估了一个不存在的东西。
    // 丢样本对 SP 800-90B 没有影响：它评估的是**独立同分布/非独立同分布**
    // 的样本序列性质，中间整段缺失只相当于换了一段采集窗口。
    generate if (RAW_TAP != 0) begin : g_rawtap
        reg  [31:0] raw_sh;
        reg  [4:0]  raw_cnt;
        reg         raw_push;
        reg  [31:0] raw_word;
        wire        raw_wr_ready;

        always @(posedge clk or negedge rst_n) begin
            if (!rst_n) begin
                raw_sh <= 32'd0; raw_cnt <= 5'd0;
                raw_push <= 1'b0; raw_word <= 32'd0;
            end else begin
                raw_push <= 1'b0;
                if (zeroize_active) begin
                    raw_sh <= 32'd0; raw_cnt <= 5'd0; raw_word <= 32'd0;
                end else if (src_valid) begin
                    raw_sh  <= {raw_sh[30:0], src_bit};
                    raw_cnt <= raw_cnt + 5'd1;          // 自然回绕 = 每 32 个一组
                    if (raw_cnt == 5'd31) begin
                        raw_word <= {raw_sh[30:0], src_bit};
                        raw_push <= 1'b1;
                    end
                end
            end
        end

        sync_fifo #(.WIDTH(32), .DEPTH(64), .WIPE_ON_FLUSH(1)) u_rawfifo (
            .clk(clk), .rst_n(rst_n),
            .flush(zeroize),
            .wr_en(raw_push && raw_wr_ready), .wr_data(raw_word),
            .wr_ready(raw_wr_ready),
            .rd_en(raw_rd_en), .rd_data(raw_data), .rd_valid(raw_valid),
            .wiping(), .level());
    end else begin : g_norawtap
        // 关掉的时候是**真的没有这条通路**，不是"读了返回 0"。
        assign raw_data  = 32'd0;
        assign raw_valid = 1'b0;
        wire _unused_raw = &{1'b0, raw_rd_en, 1'b0};
    end endgenerate

    // ---- 连续健康检测 ----
    // 吃的是抽取之后的样本流，也就是调理器实际消费的那一条 —— 检测的对象
    // 必须和被使用的对象是同一个，否则检测没有意义。
    wire health_clear = clear_alarm || zeroize;

    trng_health #(
        .SAMPLE_W(1),
        .RCT_CUTOFF(RCT_CUTOFF),
        .APT_WINDOW(APT_WINDOW),
        .APT_CUTOFF(APT_CUTOFF)
    ) u_health (
        .clk(clk), .rst_n(rst_n),
        .clear(health_clear),
        .sample_valid(src_valid), .sample(src_bit),
        .rct_alarm(rct_alarm), .apt_alarm(apt_alarm), .alarm(alarm),
        .rct_run(rct_run), .apt_count(apt_count), .apt_index(apt_index));

    // ---- 启动健康检测 ----
    // 连续 STARTUP_SAMPLES 个样本无告警才放行。中途一旦告警，计数清零重来。
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            startup_count <= 32'd0;
            startup_done  <= 1'b0;
        end else if (zeroize || clear_alarm || alarm) begin
            startup_count <= 32'd0;
            startup_done  <= 1'b0;
        end else if (!startup_done && src_valid) begin
            if (startup_count == STARTUP_SAMPLES[31:0] - 32'd1) begin
                startup_done <= 1'b1;
            end else begin
                startup_count <= startup_count + 32'd1;
            end
        end
    end

    // ---- 调理器的派生复位 ----
    reg cond_rst_n;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            cond_rst_n <= 1'b0;
        end else begin
            cond_rst_n <= startup_done && !alarm && !zeroize_active;
        end
    end

    wire        cond_word_valid, cond_word_ready;
    wire [31:0] cond_word;

    trng_cond #(
        .RATE_LANES(RATE_LANES),
        .ABSORB_BLOCKS(ABSORB_BLOCKS),
        .OUT_LANES(OUT_LANES)
    ) u_cond (
        .clk(clk), .rst_n(cond_rst_n),
        // 启动检测未过 / 已告警时不喂比特：那些样本一个都不该进熵池
        .bit_valid(src_valid && startup_done && !alarm),
        .bit_in(src_bit),
        .bit_ready(),
        .word_valid(cond_word_valid), .word_out(cond_word),
        .word_ready(cond_word_ready),
        .blocks_absorbed(blocks_absorbed));

    // ---- 输出 FIFO ----
    // flush 用边沿而不是电平：alarm 是锁存的电平，用电平会让擦除扫描每拍
    // 重新开始、永远走不完。
    reg  alarm_d;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) alarm_d <= 1'b0;
        else        alarm_d <= alarm;
    end
    wire alarm_rise  = alarm && !alarm_d;
    wire fifo_flush  = zeroize || clear_alarm || alarm_rise;

    wire fifo_wr_ready;

    sync_fifo #(
        .WIDTH(32), .DEPTH(FIFO_DEPTH), .WIPE_ON_FLUSH(1)
    ) u_fifo (
        .clk(clk), .rst_n(rst_n), .flush(fifo_flush),
        .wr_en(cond_word_valid), .wr_data(cond_word), .wr_ready(fifo_wr_ready),
        .rd_en(rd_en && rd_valid), .rd_data(rd_data), .rd_valid(rd_valid),
        .wiping(fifo_wiping), .level());

    assign cond_word_ready = fifo_wr_ready;
    assign ready = startup_done && !alarm && !zeroize_active && !fifo_wiping;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            words_out <= 32'd0;
        end else if (zeroize) begin
            words_out <= 32'd0;
        end else if (rd_en && rd_valid) begin
            words_out <= words_out + 32'd1;
        end
    end

endmodule

`default_nettype wire
