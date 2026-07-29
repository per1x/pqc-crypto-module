// pqc_accel_axi —— 加速器顶层：AXI4-Lite 控制 + AXI4-Stream 数据 + 算法核
//
// 这是把 include/pqchsm/accel.h 里"用 C 写出来的寄存器语义"落成真实总线接口的
// 那一层。控制面走 AXI4-Lite（寄存器读写），数据面走 AXI4-Stream（成块搬运），
// 与真实系统里 CPU 配寄存器、DMA 搬数据的分工一致。
//
// AXI 是 ARM 的公开标准。本层与算法核一样是纯可推断逻辑，不例化任何厂商的
// AXI 互连原语或 IP 封装，因此可以原样综合到不同厂商的器件上。
//
// 【数据面约定】
// 输入：每个 AXI4-Stream 包从缓冲区偏移 0 开始写；接受带 TLAST 的一拍后写指针
//       归零，下一个包重新从 0 开始。软件因此不需要单独的"复位写指针"寄存器。
// 输出：命令完成后，OUT_LEN 换算成的字数可读；主机拉高 TREADY 即可把结果取走，
//       最后一拍带 TLAST。取完读指针归零、TVALID 落下 —— 结果只能取一次，
//       再取需要重新发一次命令。
// 位宽固定 32 位，缓冲区按 32 位字寻址，字节序为小端（与 accel.h 一致）。
//
// 【支持的操作码】
//   7 / 8  NTT 正/逆变换   IN_LEN 必须为 512（256 个 16 位系数）
//   9      Keccak-f[1600]  IN_LEN 必须为 200（25 个 64 位 lane）
// 其余操作码置 STATUS.ERR 并把 ERRCODE 设为 3（该模式未实现）——
// 明确报错而不是悄悄回落到别的实现。
`default_nettype none

module pqc_accel_axi #(
    parameter [31:0] VERSION = 32'h0001_0000
) (
    input  wire        clk,
    input  wire        rst_n,

    // ---- AXI4-Lite 从机：控制/状态寄存器 ----
    input  wire [7:0]  s_axi_awaddr,
    input  wire        s_axi_awvalid,
    output wire        s_axi_awready,
    input  wire [31:0] s_axi_wdata,
    input  wire [3:0]  s_axi_wstrb,
    input  wire        s_axi_wvalid,
    output wire        s_axi_wready,
    output wire [1:0]  s_axi_bresp,
    output wire        s_axi_bvalid,
    input  wire        s_axi_bready,
    input  wire [7:0]  s_axi_araddr,
    input  wire        s_axi_arvalid,
    output wire        s_axi_arready,
    output wire [31:0] s_axi_rdata,
    output wire [1:0]  s_axi_rresp,
    output wire        s_axi_rvalid,
    input  wire        s_axi_rready,

    // ---- AXI4-Stream 从机：输入数据 ----
    input  wire [31:0] s_axis_tdata,
    input  wire        s_axis_tvalid,
    output wire        s_axis_tready,
    input  wire        s_axis_tlast,

    // ---- AXI4-Stream 主机：输出数据 ----
    output wire [31:0] m_axis_tdata,
    output wire        m_axis_tvalid,
    input  wire        m_axis_tready,
    output wire        m_axis_tlast
);
    // 缓冲区大小与 accel.h 的 ACCEL_BUF_MAX 一致：16 KiB = 4096 个 32 位字，
    // 地址位宽因此固定为 12 位。
    localparam integer BUF_WORDS = 4096;

    localparam [31:0] MODE_NTT_FWD = 32'd7;
    localparam [31:0] MODE_NTT_INV = 32'd8;
    localparam [31:0] MODE_KECCAK  = 32'd9;

    localparam [2:0] S_IDLE  = 3'd0,
                     S_LOAD  = 3'd1,
                     S_KICK  = 3'd2,
                     S_WAIT  = 3'd3,
                     S_STORE = 3'd4,
                     S_FIN   = 3'd5;

    reg [31:0] bufmem [0:BUF_WORDS-1];
    reg [11:0] wr_ptr;                     // 输入流写指针（按字）
    reg [11:0] rd_ptr;                     // 输出流读指针（按字）
    reg [11:0] out_words;                  // 本次结果的字数

    // ---- 寄存器组 ----
    wire        start, soft_reset;
    wire [31:0] mode, in_len;
    reg         done_set, err_set, out_we;
    reg  [31:0] out_len_r, errcode_r;
    reg         busy;

    axi4lite_regs #(.VERSION(VERSION)) u_regs (
        .clk(clk), .rst_n(rst_n),
        .s_axi_awaddr(s_axi_awaddr), .s_axi_awvalid(s_axi_awvalid),
        .s_axi_awready(s_axi_awready),
        .s_axi_wdata(s_axi_wdata), .s_axi_wstrb(s_axi_wstrb),
        .s_axi_wvalid(s_axi_wvalid), .s_axi_wready(s_axi_wready),
        .s_axi_bresp(s_axi_bresp), .s_axi_bvalid(s_axi_bvalid),
        .s_axi_bready(s_axi_bready),
        .s_axi_araddr(s_axi_araddr), .s_axi_arvalid(s_axi_arvalid),
        .s_axi_arready(s_axi_arready),
        .s_axi_rdata(s_axi_rdata), .s_axi_rresp(s_axi_rresp),
        .s_axi_rvalid(s_axi_rvalid), .s_axi_rready(s_axi_rready),
        .start(start), .soft_reset(soft_reset),
        // PARAM 属于寄存器契约的一部分（软件可读写），但本层实现的三个操作码
        // 都不按参数集分支，因此不接出来。
        .mode(mode), .param(), .in_len(in_len),
        .done_set(done_set), .busy(busy), .err_set(err_set),
        .out_len_in(out_len_r), .errcode_in(errcode_r), .out_we(out_we));

    // ---- 算法核 ----
    wire core_rst_n = rst_n && !soft_reset;

    reg                ntt_start, ntt_inverse, ntt_wr_en;
    reg  [7:0]         ntt_wr_addr;
    reg  signed [15:0] ntt_wr_data;
    reg  [7:0]         ntt_rd_addr;
    wire signed [15:0] ntt_rd_data;
    wire               ntt_done;

    ntt_core u_ntt (
        .clk(clk), .rst_n(core_rst_n),
        .start(ntt_start), .inverse(ntt_inverse), .done(ntt_done),
        .wr_en(ntt_wr_en), .wr_addr(ntt_wr_addr), .wr_data(ntt_wr_data),
        .rd_addr(ntt_rd_addr), .rd_data(ntt_rd_data));

    reg          kec_start, kec_wr_en;
    reg  [4:0]   kec_wr_addr;
    reg  [63:0]  kec_wr_data;
    reg  [4:0]   kec_rd_addr;
    wire [63:0]  kec_rd_data;
    wire         kec_done;

    keccak_f1600 u_keccak (
        .clk(clk), .rst_n(core_rst_n),
        .start(kec_start), .done(kec_done),
        .wr_en(kec_wr_en), .wr_addr(kec_wr_addr), .wr_data(kec_wr_data),
        .rd_addr(kec_rd_addr), .rd_data(kec_rd_data));

    // ---- 数据面握手 ----
    assign s_axis_tready = !busy;
    assign m_axis_tvalid = (out_words != 12'd0) && (rd_ptr < out_words);
    assign m_axis_tdata  = bufmem[rd_ptr];
    assign m_axis_tlast  = m_axis_tvalid && (rd_ptr == out_words - 12'd1);

    // ---- 命令状态机 ----
    // 缓冲区只在这一个 always 块里被写：输入流、结果回写共用同一组端口，
    // 避免多驱动。
    reg [2:0]  state;
    reg [9:0]  cnt;
    reg        is_ntt;
    reg [15:0] lo_lat;                    // 半字暂存（NTT 结果回写用）
    reg [31:0] lane_lo;                   // 低 32 位暂存（Keccak 装载用）

    wire mode_ntt    = (mode == MODE_NTT_FWD) || (mode == MODE_NTT_INV);
    wire mode_keccak = (mode == MODE_KECCAK);
    wire cmd_ok      = (mode_ntt && (in_len == 32'd512))
                    || (mode_keccak && (in_len == 32'd200));

    wire [11:0] widx_half = {3'd0, cnt[9:1]};              // NTT：两系数一字
    wire [31:0] load_word = bufmem[widx_half];
    wire [15:0] load_half = cnt[0] ? load_word[31:16] : load_word[15:0];
    wire [11:0] widx_lane = {2'd0, cnt[9:1], cnt[0]};      // Keccak：两字一 lane
    wire        kick_busy = ntt_start || kec_start;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n || soft_reset) begin
            state       <= S_IDLE;
            cnt         <= 10'd0;
            busy        <= 1'b0;
            done_set    <= 1'b0;
            err_set     <= 1'b0;
            out_we      <= 1'b0;
            out_len_r   <= 32'd0;
            errcode_r   <= 32'd0;
            out_words   <= 12'd0;
            rd_ptr      <= 12'd0;
            wr_ptr      <= 12'd0;
            is_ntt      <= 1'b0;
            ntt_start   <= 1'b0;
            ntt_inverse <= 1'b0;
            ntt_wr_en   <= 1'b0;
            ntt_wr_addr <= 8'd0;
            ntt_wr_data <= 16'sd0;
            ntt_rd_addr <= 8'd0;
            kec_start   <= 1'b0;
            kec_wr_en   <= 1'b0;
            kec_wr_addr <= 5'd0;
            kec_wr_data <= 64'd0;
            kec_rd_addr <= 5'd0;
            lo_lat      <= 16'd0;
            lane_lo     <= 32'd0;
        end else begin
            done_set  <= 1'b0;
            err_set   <= 1'b0;
            out_we    <= 1'b0;
            ntt_start <= 1'b0;
            kec_start <= 1'b0;
            ntt_wr_en <= 1'b0;
            kec_wr_en <= 1'b0;

            // 输入流：命令进行中不接收
            if (s_axis_tvalid && s_axis_tready) begin
                bufmem[wr_ptr] <= s_axis_tdata;
                wr_ptr <= s_axis_tlast ? 12'd0 : (wr_ptr + 12'd1);
            end

            // 输出流：取完读指针归零，TVALID 随之落下
            if (m_axis_tvalid && m_axis_tready) begin
                if (m_axis_tlast) begin
                    rd_ptr    <= 12'd0;
                    out_words <= 12'd0;
                end else begin
                    rd_ptr <= rd_ptr + 12'd1;
                end
            end

            case (state)
            S_IDLE: begin
                if (start) begin
                    if (!cmd_ok) begin
                        // 该模式未实现，或输入长度不符
                        errcode_r <= 32'd3;
                        out_len_r <= 32'd0;
                        out_we    <= 1'b1;
                        err_set   <= 1'b1;
                        done_set  <= 1'b1;
                    end else begin
                        busy        <= 1'b1;
                        is_ntt      <= mode_ntt;
                        ntt_inverse <= (mode == MODE_NTT_INV);
                        cnt         <= 10'd0;
                        rd_ptr      <= 12'd0;
                        out_words   <= 12'd0;
                        state       <= S_LOAD;
                    end
                end
            end

            S_LOAD: begin
                if (is_ntt) begin
                    // 每周期一个 16 位系数
                    ntt_wr_en   <= 1'b1;
                    ntt_wr_addr <= cnt[7:0];
                    ntt_wr_data <= $signed(load_half);
                    if (cnt == 10'd255) begin
                        cnt   <= 10'd0;
                        state <= S_KICK;
                    end else begin
                        cnt <= cnt + 10'd1;
                    end
                end else begin
                    // 两个字拼一个 64 位 lane
                    if (!cnt[0]) begin
                        lane_lo <= bufmem[widx_lane];
                    end else begin
                        kec_wr_en   <= 1'b1;
                        kec_wr_addr <= cnt[5:1];
                        kec_wr_data <= {bufmem[widx_lane], lane_lo};
                    end
                    if (cnt == 10'd49) begin
                        cnt   <= 10'd0;
                        state <= S_KICK;
                    end else begin
                        cnt <= cnt + 10'd1;
                    end
                end
            end

            S_KICK: begin
                if (is_ntt) begin
                    ntt_start <= 1'b1;
                end else begin
                    kec_start <= 1'b1;
                end
                state <= S_WAIT;
            end

            S_WAIT: begin
                // 核的 done 是电平语义，上一次运算的残留会一直保持。
                // kick_busy 高的那一拍正是核看到 start 的那一拍，此时 done 尚未清，
                // 必须跳过；下一拍起 done 才反映本次运算。
                if (!kick_busy && (is_ntt ? ntt_done : kec_done)) begin
                    cnt         <= 10'd0;
                    ntt_rd_addr <= 8'd0;
                    kec_rd_addr <= 5'd0;
                    state       <= S_STORE;
                end
            end

            S_STORE: begin
                if (is_ntt) begin
                    // 组合读，地址提前一拍给出
                    ntt_rd_addr <= cnt[7:0] + 8'd1;
                    if (!cnt[0]) begin
                        lo_lat <= ntt_rd_data;
                    end else begin
                        bufmem[widx_half] <= {ntt_rd_data, lo_lat};
                    end
                    if (cnt == 10'd255) begin
                        out_len_r <= 32'd512;
                        out_words <= 12'd128;
                        state     <= S_FIN;
                    end else begin
                        cnt <= cnt + 10'd1;
                    end
                end else begin
                    kec_rd_addr <= cnt[5:1] + {4'd0, cnt[0]};
                    bufmem[{2'd0, cnt}] <=
                        cnt[0] ? kec_rd_data[63:32] : kec_rd_data[31:0];
                    if (cnt == 10'd49) begin
                        out_len_r <= 32'd200;
                        out_words <= 12'd50;
                        state     <= S_FIN;
                    end else begin
                        cnt <= cnt + 10'd1;
                    end
                end
            end

            S_FIN: begin
                errcode_r <= 32'd0;
                out_we    <= 1'b1;
                err_set   <= 1'b0;
                done_set  <= 1'b1;
                busy      <= 1'b0;
                state     <= S_IDLE;
            end

            default: state <= S_IDLE;
            endcase
        end
    end
endmodule

`default_nettype wire
