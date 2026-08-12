// trng_axi —— TRNG 的 AXI4-Lite 从机（带 AxPROT 安全门控）
//
// 【为什么 TRNG 单独挂一个从机，而不是并进 pqc_accel_axi 的操作码】
// 真密码机里熵源是独立外设，理由有三条，每条都是架构性的：
//   · 生命周期不同：加速器是"发命令→等完成"，TRNG 是常开自由运行的；
//   · 访问权限不同：熵源通常比算法加速器管得更严；
//   · 故障域不同：TRNG 告警要能独立上报，不该被加速器的忙闲状态挡住。
// 并进操作码会把这三件事搅在一起。
//
// ============================================================================
// 【AxPROT 门控：这是"只让安全 master 访问"的硬件落点】
// ============================================================================
// AXI 的 AxPROT[1] 是 NS 位：0 = secure，1 = non-secure。ZynqMP 的 PS 会把
// 发起访问的 master 的安全属性透到这一位上，PL 侧可以直接判。
//
// SECURE_ONLY=1 时，non-secure 的读写一律返回 **DECERR**，且**不产生任何副作用**
// —— 尤其是不弹 FIFO。返回 DECERR 而不是 OKAY+0 是有意的：静默返回 0 会让
// 普通世界以为自己拿到了随机数，而 0 是最糟的"随机数"。报错让它立刻知道。
//
// 这一层是**纵深防御的最内层，不是唯一一层**。真正的隔离还要靠：
//   · XMPU/XPPU 在 PS 侧拦住到这段地址的非法访问（在 master 那端就挡掉）；
//   · 地址映射本身不出现在普通世界的设备树里。
// 三层都做，是因为任何一层单独都可能被绕：AxPROT 判的是总线上的位，
// 如果某个 master 被配置成永远发 secure，这一层就形同虚设 —— 那正是
// XPPU 该管的事。docs/ 里的隔离设计文档会把三层的分工写清楚。
//
// ============================================================================
// 【寄存器表】（偏移，宽度均 32 位）
//   0x00 CTRL      RW  [0]=ENABLE(电平) [1]=ZEROIZE(写1脉冲) [2]=CLEAR_ALARM(写1脉冲)
//   0x04 STATUS    R   [0]=READY      [1]=DATA_VALID [2]=ALARM   [3]=RCT_ALARM
//                      [4]=APT_ALARM  [5]=STARTUP_DONE [6]=FIFO_WIPING
//                      [7]=ENABLED    [8]=UNDERRUN(锁存，写 CTRL.CLEAR_ALARM 清)
//   0x08 RDATA     R   **读一次弹出一个随机字**。FIFO 空时返回 0 并置 UNDERRUN。
//   0x0C HEALTH    R   {apt_count[31:16], rct_run[15:0]}
//   0x10 APT_INDEX R   本 APT 窗口已处理的样本数
//   0x14 STARTUP   R   启动健康检测已通过的样本数
//   0x18 BLOCKS    R   调理器已吸收的 rate 块数
//   0x1C WORDS     R   已交付给软件的字数
//   0x20 VERSION   R
//   0x24 PARAM0    R   {DECIM[7:0], NUM_RO[7:0], RATE_LANES[7:0], OUT_LANES[7:0]}
//   0x28 PARAM1    R   {APT_CUTOFF[15:0], RCT_CUTOFF[15:0]}
//   0x2C PARAM2    R   {STARTUP_SAMPLES[15:0], APT_WINDOW[15:0]}
//
// PARAM0/1/2 是只读的参数回读口。软件启动自测时用它核对"硬件里跑的阈值"
// 与"驱动以为的阈值"一致 —— 这类不一致在真系统里出过事：改了 RTL 参数
// 忘了改驱动，健康检测看起来正常，实际判据已经不是标准要求的那个了。
//
// **RDATA 是读时弹出的**。AXI4-Lite 没有推测读，读一次就是弹一次，
// 软件不能对同一地址做"读回校验"这种事。
`default_nettype none

module trng_axi #(
    parameter [31:0]  VERSION         = 32'h0001_0000,
    parameter integer SECURE_ONLY     = 1,     // 1 = non-secure 访问返回 DECERR

    // 透传给 trng_top
    parameter integer NUM_RO          = 8,
    parameter integer RO_STAGES_0     = 13,
    parameter integer DECIM           = 8,
    parameter integer RCT_CUTOFF      = 47,    // 实测 H=0.871234，见 trng_health.v
    parameter integer APT_WINDOW      = 1024,
    parameter integer APT_CUTOFF      = 672,
    parameter integer STARTUP_SAMPLES = 1024,
    parameter integer RATE_LANES      = 17,
    parameter integer ABSORB_BLOCKS   = 1,
    parameter integer OUT_LANES       = 4,
    parameter integer FIFO_DEPTH      = 16,
    // 原始噪声抽头。默认关。打开的构建**只用于跑 SP 800-90B 取数**，
    // 取完换回 0 —— 理由见 trng_top.v 里 RAW_TAP 的那段注释。
    parameter integer RAW_TAP         = 0
) (
    input  wire        clk,
    input  wire        rst_n,

    // ---- AXI4-Lite 从机 ----
    input  wire [7:0]  s_axi_awaddr,
    input  wire [2:0]  s_axi_awprot,
    input  wire        s_axi_awvalid,
    output wire        s_axi_awready,
    input  wire [31:0] s_axi_wdata,
    input  wire [3:0]  s_axi_wstrb,
    input  wire        s_axi_wvalid,
    output wire        s_axi_wready,
    output reg  [1:0]  s_axi_bresp,
    output reg         s_axi_bvalid,
    input  wire        s_axi_bready,

    input  wire [7:0]  s_axi_araddr,
    input  wire [2:0]  s_axi_arprot,
    input  wire        s_axi_arvalid,
    output wire        s_axi_arready,
    output reg  [31:0] s_axi_rdata,
    output reg  [1:0]  s_axi_rresp,
    output reg         s_axi_rvalid,
    input  wire        s_axi_rready,

    // ---- 篡改检测输入：拉高即擦除全部中间态 ----
    // 密码边界的 zeroize-on-tamper 挂钩点。板级的机箱开盖、电压/温度越界、
    // JTAG 插入等检测信号接到这里。它与软件写 CTRL.ZEROIZE 是或的关系 ——
    // 硬件通路不经过软件，软件被攻陷也擦得掉。
    input  wire        tamper,

    // ---- 观测口（接到 ILA / LED，不出芯片边界） ----
    output wire        trng_ready,
    output wire        trng_alarm
);
    localparam [1:0] RESP_OKAY   = 2'b00;
    localparam [1:0] RESP_DECERR = 2'b11;

    localparam [3:0] A_CTRL    = 4'h0, A_STATUS  = 4'h1, A_RDATA   = 4'h2,
                     A_HEALTH  = 4'h3, A_APTIDX  = 4'h4, A_STARTUP = 4'h5,
                     A_BLOCKS  = 4'h6, A_WORDS   = 4'h7, A_VERSION = 4'h8,
                     A_PARAM0  = 4'h9, A_PARAM1  = 4'hA, A_PARAM2  = 4'hB,
                     A_RAW     = 4'hC;   // 原始噪声，RAW_TAP=1 时才有东西

    // ---- TRNG 本体 ----
    reg         reg_enable;
    reg         pulse_zeroize, pulse_clear;
    wire        fifo_rd_en;
    wire [31:0] fifo_rd_data;
    wire        fifo_rd_valid;
    wire        ready, alarm, rct_alarm, apt_alarm, startup_done, fifo_wiping;
    wire [15:0] rct_run, apt_count, apt_index;
    wire [31:0] startup_count, blocks_absorbed, words_out;
    wire        raw_rd_en;
    wire [31:0] raw_data;
    wire        raw_valid;

    trng_top #(
        .NUM_RO(NUM_RO), .RO_STAGES_0(RO_STAGES_0), .DECIM(DECIM),
        .RCT_CUTOFF(RCT_CUTOFF), .APT_WINDOW(APT_WINDOW),
        .APT_CUTOFF(APT_CUTOFF), .STARTUP_SAMPLES(STARTUP_SAMPLES),
        .RATE_LANES(RATE_LANES), .ABSORB_BLOCKS(ABSORB_BLOCKS),
        .OUT_LANES(OUT_LANES), .FIFO_DEPTH(FIFO_DEPTH), .RAW_TAP(RAW_TAP)
    ) u_trng (
        .clk(clk), .rst_n(rst_n),
        .enable(reg_enable),
        .zeroize(pulse_zeroize || tamper),
        .clear_alarm(pulse_clear),
        .rd_en(fifo_rd_en), .rd_data(fifo_rd_data), .rd_valid(fifo_rd_valid),
        .ready(ready), .alarm(alarm),
        .rct_alarm(rct_alarm), .apt_alarm(apt_alarm),
        .startup_done(startup_done), .fifo_wiping(fifo_wiping),
        .rct_run(rct_run), .apt_count(apt_count), .apt_index(apt_index),
        .startup_count(startup_count), .blocks_absorbed(blocks_absorbed),
        .words_out(words_out),
        .raw_rd_en(raw_rd_en), .raw_data(raw_data), .raw_valid(raw_valid));

    assign trng_ready = ready;
    assign trng_alarm = alarm;

    // ---- 写通道 ----
    reg        aw_hold, w_hold;
    reg [7:0]  aw_addr;
    reg [2:0]  aw_prot;
    reg [31:0] w_data;
    reg [3:0]  w_strb;

    wire aw_go = s_axi_awvalid && !aw_hold;
    wire w_go  = s_axi_wvalid  && !w_hold;
    assign s_axi_awready = !aw_hold;
    assign s_axi_wready  = !w_hold;

    wire        wr_fire = (aw_hold || aw_go) && (w_hold || w_go) && !s_axi_bvalid;
    wire [7:0]  wr_addr = aw_hold ? aw_addr : s_axi_awaddr;
    wire [2:0]  wr_prot = aw_hold ? aw_prot : s_axi_awprot;
    wire [31:0] wr_data = w_hold  ? w_data  : s_axi_wdata;
    wire [3:0]  wr_strb = w_hold  ? w_strb  : s_axi_wstrb;

    // AxPROT[1] = NS 位：0 是 secure。SECURE_ONLY=0 时这一层直通。
    wire wr_permit = (SECURE_ONLY == 0) || (wr_prot[1] == 1'b0);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            aw_hold       <= 1'b0;
            w_hold        <= 1'b0;
            aw_addr       <= 8'd0;
            aw_prot       <= 3'd0;
            w_data        <= 32'd0;
            w_strb        <= 4'd0;
            s_axi_bvalid  <= 1'b0;
            s_axi_bresp   <= RESP_OKAY;
            reg_enable    <= 1'b1;   // 上电即开始暖机：启动健康检测本来就要时间
            pulse_zeroize <= 1'b0;
            pulse_clear   <= 1'b0;
        end else begin
            pulse_zeroize <= 1'b0;
            pulse_clear   <= 1'b0;

            if (aw_go && !wr_fire) begin
                aw_hold <= 1'b1;
                aw_addr <= s_axi_awaddr;
                aw_prot <= s_axi_awprot;
            end
            if (w_go && !wr_fire) begin
                w_hold <= 1'b1;
                w_data <= s_axi_wdata;
                w_strb <= s_axi_wstrb;
            end

            if (wr_fire) begin
                aw_hold      <= 1'b0;
                w_hold       <= 1'b0;
                s_axi_bvalid <= 1'b1;
                // 被门控的写：握手照常完成（否则总线挂死），但报 DECERR 且不落笔
                s_axi_bresp  <= wr_permit ? RESP_OKAY : RESP_DECERR;

                if (wr_permit && (wr_addr[5:2] == A_CTRL) && wr_strb[0]) begin
                    reg_enable    <= wr_data[0];
                    pulse_zeroize <= wr_data[1];
                    pulse_clear   <= wr_data[2];
                end
            end

            if (s_axi_bvalid && s_axi_bready) begin
                s_axi_bvalid <= 1'b0;
            end
        end
    end

    // ---- 读通道 ----
    wire ar_permit = (SECURE_ONLY == 0) || (s_axi_arprot[1] == 1'b0);

    assign s_axi_arready = !s_axi_rvalid;
    wire ar_go = s_axi_arvalid && s_axi_arready;

    // 只有被允许的、地址命中 RDATA 的、且 FIFO 有数的读才弹出。
    // 被 DECERR 挡掉的读绝不能弹 —— 否则一个 non-secure 的读虽然拿不到数，
    // 却把一个随机字冲掉了，等于给了普通世界一个耗尽熵池的手段。
    assign fifo_rd_en = ar_go && ar_permit
                     && (s_axi_araddr[5:2] == A_RDATA) && fifo_rd_valid;

    // 原始噪声口同一条纪律：被 DECERR 挡掉的读不弹出。
    // 这里更要紧 —— 原始比特是熵源的内部状态，让一个拿不到数据的读把它
    // 冲掉，等于把"取样"和"取到"拆开了，采集程序会以为自己拿到了连续的流。
    assign raw_rd_en = ar_go && ar_permit
                    && (s_axi_araddr[5:2] == A_RAW) && raw_valid;

    reg underrun;

    wire [31:0] status_word = {22'd0,
                               raw_valid,       // [9] 原始噪声 FIFO 里有字
                               underrun,        // [8]
                               reg_enable,      // [7]
                               fifo_wiping,     // [6]
                               startup_done,    // [5]
                               apt_alarm,       // [4]
                               rct_alarm,       // [3]
                               alarm,           // [2]
                               fifo_rd_valid,   // [1]
                               ready};          // [0]

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            s_axi_rvalid <= 1'b0;
            s_axi_rresp  <= RESP_OKAY;
            s_axi_rdata  <= 32'd0;
            underrun     <= 1'b0;
        end else begin
            if (pulse_clear || pulse_zeroize) begin
                underrun <= 1'b0;
            end

            if (ar_go) begin
                s_axi_rvalid <= 1'b1;
                s_axi_rresp  <= ar_permit ? RESP_OKAY : RESP_DECERR;

                if (!ar_permit) begin
                    s_axi_rdata <= 32'd0;
                end else begin
                    case (s_axi_araddr[5:2])
                    A_CTRL:    s_axi_rdata <= {31'd0, reg_enable};
                    A_STATUS:  s_axi_rdata <= status_word;
                    A_RDATA: begin
                        // 空读返回 0 并锁存 UNDERRUN。返回 0 本身是危险的
                        // （软件若不看状态就会把 0 当随机数用），所以驱动
                        // 必须先查 STATUS.DATA_VALID —— UNDERRUN 就是用来
                        // 在事后抓住"没查就读"这种驱动 bug 的。
                        s_axi_rdata <= fifo_rd_valid ? fifo_rd_data : 32'd0;
                        if (!fifo_rd_valid) begin
                            underrun <= 1'b1;
                        end
                    end
                    A_HEALTH:  s_axi_rdata <= {apt_count, rct_run};
                    A_APTIDX:  s_axi_rdata <= {16'd0, apt_index};
                    A_STARTUP: s_axi_rdata <= startup_count;
                    A_BLOCKS:  s_axi_rdata <= blocks_absorbed;
                    A_WORDS:   s_axi_rdata <= words_out;
                    A_RAW: begin
                        // RAW_TAP=0 时这里恒为 0 —— 但那不是"读了返回 0"，
                        // 而是整条通路在综合时就不存在（见 trng_top.v）。
                        s_axi_rdata <= raw_valid ? raw_data : 32'd0;
                    end
                    A_VERSION: s_axi_rdata <= VERSION;
                    A_PARAM0:  s_axi_rdata <= {DECIM[7:0], NUM_RO[7:0],
                                               RATE_LANES[7:0], OUT_LANES[7:0]};
                    A_PARAM1:  s_axi_rdata <= {APT_CUTOFF[15:0], RCT_CUTOFF[15:0]};
                    A_PARAM2:  s_axi_rdata <= {STARTUP_SAMPLES[15:0],
                                               APT_WINDOW[15:0]};
                    default:   s_axi_rdata <= 32'd0;
                    endcase
                end
            end else if (s_axi_rvalid && s_axi_rready) begin
                s_axi_rvalid <= 1'b0;
            end
        end
    end

endmodule

`default_nettype wire
