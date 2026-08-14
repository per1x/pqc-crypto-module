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
//   10     SHAKE / SHA3    IN_LEN = 消息字节数；PARAM 见下
// 其余操作码置 STATUS.ERR 并把 ERRCODE 设为 3（该模式未实现）——
// 明确报错而不是悄悄回落到别的实现。
//
// 【模式 10 的 PARAM 编码】
//   [7:0]   域分隔后缀：SHAKE=0x1F，SHA3=0x06
//   [15:8]  rate 字节数：SHAKE128=168，SHAKE256/SHA3-256=136，SHA3-512=72
//   [31:16] 请求的输出字节数
// 输出长度放 PARAM 而不是另加寄存器，是为了不动 docs/REGISTERS.md 那张
// 已经定死的表 —— PARAM 本来就是"参数集"字段，本层此前没用过它。
//
// 模式 9 与模式 10 **共用同一个 keccak_f1600**：模式 10 用 sha3_core 的海绵，
// 模式 9 走 sha3_core 的直通口借用它底下的置换核。一次只有一条命令在跑，
// 所以不存在争用；而各占一个置换核要多花约 3500 LUT，ZU3EG 上还得留给 S4 的
// ML-KEM 核。
//
// 【模式 10 结束时会擦海绵】命令收尾会给 sha3_core 打一拍 zeroize 并等它清完，
// 理由有两条：一是海绵挤压完不会自己回空闲（SHAKE 输出长度任意，核心不知道
// 上层何时读够），不擦的话直通口就一直借不出去，下一条模式 9 命令会挂死；
// 二是留在 lane 里的挤压状态本来也不该跨命令存活。
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
    // 【缓冲区按已实现的操作码定尺寸，而不是照抄软件侧的上界】
    // accel.h 的 ACCEL_BUF_MAX 是 16 KiB，那是软件侧要容纳完整 ML-KEM/ML-DSA
    // 输入的上界；本层实现的操作码里输入最大的是 NTT（512 字节），因此硬件
    // 缓冲区取 512 字节 = 128 个 32 位字。模式 10 的消息与摘要也按这个上限
    // 卡（各 ≤512 字节）—— 超出的由软件侧退回 C 侧海绵，见 accel_shake()。
    //
    // 这不是省资源的小优化：这块存储是双读口（输出流一路、装载一路）加写口，
    // 综合工具无法把它映射成块 RAM，只能摊成寄存器或分布式 RAM。按 16 KiB 写
    // 就是 131072 个触发器 —— 超过 XC7Z020 全片 106400 个触发器，根本放不下。
    //
    // ⚠️ 这里原来写着"按 128 字定尺寸后映射成分布式 RAM 只占几十个 LUT"。
    // **实测不成立。** 在 xazu3eg-sfvc784-1-i 上把本模块整个综合 + 布线之后，
    // 报告里 LUT as Memory = 0 —— Vivado 一个单元都没映射成分布式 RAM，
    // 全摊成了组合选择树（F7 Muxes 5434 / F8 Muxes 2462）。原因是读口不止一个，
    // 且模式 10 还要按**字节**读写，粒度对不上 LUTRAM 的端口结构。
    // 这块缓冲区实测占约 30000 LUT，比 ntt_core 还多，是本模块吃掉 88.89% 片子
    // 的两个大头之一（另一个是 ntt_core 的系数寄存器阵列）。
    //
    // S3 已经改成一块真双口 BRAM（common/ram_dp.v）：
    //   A 口 —— 输入流写入、装载读出、结果写回，按状态分时复用；
    //   B 口 —— 输出流只读。
    // 代价是读口全部变成同步读，所以下面每条读路径都要"地址提前一拍"，
    // 模式 10 的挤压也从"按字节读-改-写"改成"攒满一个字再整字写"。
    localparam integer BUF_WORDS = 128;
    localparam integer BUF_AW    = 7;

    localparam [31:0] MODE_NTT_FWD = 32'd7;
    localparam [31:0] MODE_NTT_INV = 32'd8;
    localparam [31:0] MODE_KECCAK  = 32'd9;
    localparam [31:0] MODE_SHAKE   = 32'd10;

    localparam [3:0] S_IDLE      = 4'd0,
                     S_LOAD      = 4'd1,
                     S_KICK      = 4'd2,
                     S_WAIT      = 4'd3,
                     S_STORE     = 4'd4,
                     S_FIN       = 4'd5,
                     S_SHK_KICK  = 4'd6,
                     S_SHK_ABS   = 4'd7,
                     S_SHK_FLUSH = 4'd8,
                     S_SHK_SQ    = 4'd9,
                     S_SHK_WIPE  = 4'd10,
                     S_NTT_PRE   = 4'd11,   // 等 ntt_core 的同步读延迟那一拍
                     S_LOAD_PRE  = 4'd12;   // 等缓冲区 BRAM 的同步读延迟那一拍

    // ---- 数据缓冲区的两个 BRAM 端口 ----
    // 端口归属完全由状态决定，见下面那个 always @(*) 的 mux。
    reg  [BUF_AW-1:0] bufa_addr;
    reg               bufa_we;
    reg  [31:0]       bufa_din;
    wire [31:0]       bufa_dout;
    wire [BUF_AW-1:0] bufb_addr;
    wire [31:0]       bufb_dout;

    reg [8:0]  wr_ptr;                     // 输入流写指针（按字）
    reg [8:0]  rd_ptr;                     // 输出流读指针（按字）
    reg [8:0]  out_words;                  // 本次结果的字数

    // ---- 寄存器组 ----
    wire        start, soft_reset;
    wire [31:0] mode, in_len, param_w;
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
        // PARAM 在 NTT / Keccak 三个操作码下不用（它们不按参数集分支），
        // 模式 10 拿它当 {输出长度, rate, suffix} 用。
        .mode(mode), .param(param_w), .in_len(in_len),
        .done_set(done_set), .busy(busy), .err_set(err_set),
        .out_len_in(out_len_r), .errcode_in(errcode_r), .out_we(out_we));

    // ---- 算法核 ----
    // 核的复位口寄存一拍再驱动，而不是把 soft_reset 组合进异步复位网络：
    // 组合出来的异步复位是有毛刺风险的，综合工具也会就此报 DRC。
    reg core_rst_n;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            core_rst_n <= 1'b0;
        end else begin
            core_rst_n <= !soft_reset;
        end
    end

    // 状态机的状态与计数器提前声明：模式 10 的握手线（in_valid / out_ready）
    // 由它们组合出来，而这两根线要接到下面的核例化上。
    reg [3:0]  state;
    reg [9:0]  cnt;
    reg        is_ntt;
    reg [15:0] lo_lat;                    // 半字暂存（NTT 结果回写用）
    reg [31:0] lane_lo;                   // 低 32 位暂存（Keccak 装载用）

    // 缓冲区的按字节取用（模式 10）。cnt 既当吸收下标又当挤压下标 ——
    // 消息全部吸收完才开始挤压，两个阶段不重叠，所以能共用一个计数器，
    // 也因此挤压的结果覆盖掉输入消息是安全的。
    wire [BUF_AW-1:0] byte_widx   = cnt[BUF_AW+1:2];
    wire [4:0]        byte_sh     = {cnt[1:0], 3'b000};
    wire [7:0]        shk_in_byte = bufa_dout[byte_sh +: 8];

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

    // 模式 9 的裸置换：不再自己例化 keccak_f1600，改成向 sha3_core 借 ——
    // 端口名与语义与直接例化时完全一致，所以下面的状态机一行没改。
    reg          kec_start, kec_wr_en;
    reg  [4:0]   kec_wr_addr;
    reg  [63:0]  kec_wr_data;
    reg  [4:0]   kec_rd_addr;
    wire [63:0]  kec_rd_data;
    wire         kec_done;

    // 模式 10 的海绵
    reg          shk_start, shk_flush, shk_zeroize;
    wire         shk_in_ready, shk_out_valid, shk_busy;
    wire [7:0]   shk_out_data;
    reg  [7:0]   shk_rate, shk_suffix;
    reg  [9:0]   shk_msglen, shk_outlen;

    // 输出字数（向上取整到 32 位字）。写成独立的 wire 是因为 Verilog-2001
    // 不允许对表达式直接做位选，写在赋值语句里会编译不过。
    wire [9:0]   shk_outw = (shk_outlen + 10'd3) >> 2;

    wire         shk_in_valid  = (state == S_SHK_ABS) && (cnt != shk_msglen);
    wire         shk_out_ready = (state == S_SHK_SQ)  && (cnt != shk_outlen);

    sha3_core u_sha3 (
        .clk(clk), .rst_n(core_rst_n),
        .rate_bytes(shk_rate), .suffix(shk_suffix),
        .start(shk_start), .zeroize(shk_zeroize),
        .in_valid(shk_in_valid), .in_ready(shk_in_ready), .in_data(shk_in_byte),
        .in_flush(shk_flush),
        .out_valid(shk_out_valid), .out_ready(shk_out_ready),
        .out_data(shk_out_data),
        .busy(shk_busy), .absorbing(), .squeezing(),
        .ext_start(kec_start), .ext_done(kec_done),
        .ext_wr_en(kec_wr_en), .ext_wr_addr(kec_wr_addr),
        .ext_wr_data(kec_wr_data),
        .ext_rd_addr(kec_rd_addr), .ext_rd_data(kec_rd_data));

    // ---- 数据面握手 ----
    assign s_axis_tready = !busy;
    assign m_axis_tvalid = (out_words != 9'd0) && (rd_ptr < out_words);
    assign m_axis_tlast  = m_axis_tvalid && (rd_ptr == out_words - 9'd1);

    // 输出流读的是 B 口，同步读。标准的"读超前一拍"写法：地址永远给
    // **下一拍的** rd_ptr，于是本拍 bufb_dout 恰好是本拍 rd_ptr 指的那个字。
    // 没有握手时 rd_ptr_nxt == rd_ptr，B 口就一直重读同一个字，
    // 所以命令做完、TVALID 刚拉起来的那一拍数据也是对的。
    wire [8:0] rd_ptr_nxt = (m_axis_tvalid && m_axis_tready)
                          ? (m_axis_tlast ? 9'd0 : (rd_ptr + 9'd1))
                          : rd_ptr;
    assign bufb_addr    = rd_ptr_nxt[BUF_AW-1:0];
    assign m_axis_tdata = bufb_dout;

    // ---- 命令状态机 ----
    // 缓冲区只在这一个 always 块里被写：输入流、结果回写共用同一组端口，
    // 避免多驱动。
    wire mode_ntt    = (mode == MODE_NTT_FWD) || (mode == MODE_NTT_INV);
    wire mode_keccak = (mode == MODE_KECCAK);
    wire mode_shake  = (mode == MODE_SHAKE);

    // 模式 10 的 PARAM 拆解
    wire [7:0]  p_suffix = param_w[7:0];
    wire [7:0]  p_rate   = param_w[15:8];
    wire [15:0] p_outlen = param_w[31:16];

    // 参数校验就地做完，不进状态机 ——
    // rate 必须是 8 的倍数（pad 的位置计算全建立在这上面），消息与输出都
    // 不能超过这块 512 字节的缓冲区。校验不过就与"未实现的模式"一样报
    // ERRCODE=3，因为对调用方来说都是"这条命令这台加速器干不了"。
    wire shake_ok = mode_shake
                 && (p_rate != 8'd0) && (p_rate[2:0] == 3'd0) && (p_rate <= 8'd200)
                 && (p_outlen != 16'd0) && (p_outlen <= 16'd512)
                 && (in_len <= 32'd512);

    wire cmd_ok      = (mode_ntt && (in_len == 32'd512))
                    || (mode_keccak && (in_len == 32'd200))
                    || shake_ok;


    wire [BUF_AW-1:0] widx_half = cnt[7:1];               // NTT：两系数一字
    wire [15:0] load_half = cnt[0] ? bufa_dout[31:16] : bufa_dout[15:0];
    // Keccak 是一字一次，地址就是 cnt[BUF_AW-1:0]，在下面的 bufa_addr 里直接写；
    // 原先这里还有一根同义的 widx_lane，自从那处改成写 cnt 之后就没人用了。
    wire        kick_busy = ntt_start || kec_start;

    // ---- 模式 10 挤压：攒满一个 32 位字再整字写回 ----
    // 原来是"按字节读-改-写"：同一拍既读 bufmem[byte_widx] 又写回去。
    // BRAM 做不到这件事（读要一拍延迟），而且那条读路径本身就是把缓冲区
    // 摊成 LUT 选择树的原因之一。改成字节攒进 sq_acc、够 4 个（或到输出末尾）
    // 才写一次，读口直接消失。
    // 副作用是好的：最后那个不满 4 字节的字，多出来的字节现在是 0，
    // 而不是残留的输入消息字节。
    reg [31:0] sq_acc;
    wire [31:0] sq_next = ((cnt[1:0] == 2'd0) ? 32'd0 : sq_acc)
                        | ({24'd0, shk_out_data} << byte_sh);
    wire        sq_flush = (cnt[1:0] == 2'd3) || ((cnt + 10'd1) == shk_outlen);

    wire [9:0]  cnt_p1 = cnt + 10'd1;      // 下一拍的计数器（读地址提前一拍用）

    ram_dp #(.DW(32), .AW(BUF_AW)) u_buf (
        .clk    (clk),
        .a_we   (bufa_we), .a_addr(bufa_addr), .a_din(bufa_din), .a_dout(bufa_dout),
        .b_we   (1'b0),    .b_addr(bufb_addr), .b_din(32'd0),    .b_dout(bufb_dout)
    );

    // ---- A 口的归属：完全由状态决定 ----
    // 读路径一律"地址提前一拍"：本状态里给出的地址，下一拍才在 bufa_dout 上。
    // 所以 S_LOAD / S_SHK_ABS 里给的是**下一拍要用**的地址，
    // 进入这两个状态之前分别由 S_LOAD_PRE / S_SHK_KICK 把第 0 个地址先发出去。
    //
    // A 口与 B 口读同一个地址时，Xilinx BRAM 的"跨口读写冲突"是未定义行为。
    // 这里不会碰上：B 口在 out_words 非零之前一直停在字 0，而各条写回路径
    // 设置 out_words 的那一拍写的都不是字 0。
    always @(*) begin
        bufa_we   = 1'b0;
        bufa_addr = {BUF_AW{1'b0}};
        bufa_din  = 32'd0;
        case (state)
        S_IDLE: begin
            // 输入流写入。命令一开跑 s_axis_tready 就落，与下面几条路不会撞。
            bufa_we   = s_axis_tvalid && s_axis_tready
                        && (wr_ptr < BUF_WORDS[8:0]);
            bufa_addr = wr_ptr[BUF_AW-1:0];
            bufa_din  = s_axis_tdata;
        end

        S_LOAD_PRE: bufa_addr = {BUF_AW{1'b0}};

        // 装载：NTT 两个系数一个字（下一拍要 (cnt+1)>>1），
        //       Keccak 一个字一拍（下一拍要 cnt+1）。
        S_LOAD: bufa_addr = is_ntt ? cnt_p1[7:1] : cnt_p1[6:0];

        S_STORE: begin
            if (is_ntt) begin
                bufa_we   = cnt[0];
                bufa_addr = widx_half;
                bufa_din  = {ntt_rd_data, lo_lat};
            end else begin
                bufa_we   = 1'b1;
                bufa_addr = cnt[BUF_AW-1:0];
                bufa_din  = cnt[0] ? kec_rd_data[63:32] : kec_rd_data[31:0];
            end
        end

        S_SHK_KICK: bufa_addr = {BUF_AW{1'b0}};

        // 吸收：cnt 只在握上手的那一拍前进，地址跟着"下一拍的 cnt"走。
        S_SHK_ABS: bufa_addr = shk_in_ready ? cnt_p1[8:2] : cnt[8:2];

        S_SHK_SQ: begin
            bufa_we   = shk_out_valid && (cnt != shk_outlen) && sq_flush;
            bufa_addr = byte_widx;
            bufa_din  = sq_next;
        end

        default: ;
        endcase
    end

    // 异步复位分支里只允许出现 rst_n 本身：把 soft_reset 一起写进去，
    // 会让综合工具看到一个不在敏感表里的复位条件，行为与仿真不一致。
    // soft_reset 因此走独立的同步分支，复位到与上电相同的取值。
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state       <= S_IDLE;
            cnt         <= 10'd0;
            busy        <= 1'b0;
            done_set    <= 1'b0;
            err_set     <= 1'b0;
            out_we      <= 1'b0;
            out_len_r   <= 32'd0;
            errcode_r   <= 32'd0;
            out_words   <= 9'd0;
            rd_ptr      <= 9'd0;
            wr_ptr      <= 9'd0;
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
            sq_acc      <= 32'd0;
            shk_start   <= 1'b0;
            shk_flush   <= 1'b0;
            shk_zeroize <= 1'b0;
            shk_rate    <= 8'd136;
            shk_suffix  <= 8'h1F;
            shk_msglen  <= 10'd0;
            shk_outlen  <= 10'd0;
        end else if (soft_reset) begin
            state       <= S_IDLE;
            cnt         <= 10'd0;
            busy        <= 1'b0;
            done_set    <= 1'b0;
            err_set     <= 1'b0;
            out_we      <= 1'b0;
            out_len_r   <= 32'd0;
            errcode_r   <= 32'd0;
            out_words   <= 9'd0;
            rd_ptr      <= 9'd0;
            wr_ptr      <= 9'd0;
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
            sq_acc      <= 32'd0;
            shk_start   <= 1'b0;
            shk_flush   <= 1'b0;
            shk_zeroize <= 1'b0;
            shk_rate    <= 8'd136;
            shk_suffix  <= 8'h1F;
            shk_msglen  <= 10'd0;
            shk_outlen  <= 10'd0;
        end else begin
            done_set  <= 1'b0;
            err_set   <= 1'b0;
            out_we    <= 1'b0;
            ntt_start <= 1'b0;
            kec_start <= 1'b0;
            ntt_wr_en <= 1'b0;
            kec_wr_en <= 1'b0;
            shk_start   <= 1'b0;
            shk_flush   <= 1'b0;
            shk_zeroize <= 1'b0;

            // 输入流：命令进行中不接收。超出缓冲区的拍照常握手但丢弃，
            // 否则写指针回绕会把包首已经收好的字覆盖掉。
            // （写进 BRAM 那一步由上面的 A 口 mux 做，这里只推指针。）
            if (s_axis_tvalid && s_axis_tready) begin
                wr_ptr <= s_axis_tlast ? 9'd0 : (wr_ptr + 9'd1);
            end

            // 输出流：取完读指针归零，TVALID 随之落下
            if (m_axis_tvalid && m_axis_tready) begin
                if (m_axis_tlast) begin
                    rd_ptr    <= 9'd0;
                    out_words <= 9'd0;
                end else begin
                    rd_ptr <= rd_ptr + 9'd1;
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
                        rd_ptr      <= 9'd0;
                        out_words   <= 9'd0;
                        // 参数在这里锁存一次。软件不该在命令进行中改 PARAM，
                        // 但锁存之后就轮不到"不该"来兜底了。
                        shk_rate    <= p_rate;
                        shk_suffix  <= p_suffix;
                        shk_msglen  <= in_len[9:0];
                        shk_outlen  <= p_outlen[9:0];
                        state       <= mode_shake ? S_SHK_KICK : S_LOAD_PRE;
                    end
                end
            end

            // 缓冲区是 BRAM，读有一拍延迟：这一拍把字 0 的地址发出去，
            // 下一拍进 S_LOAD 时 bufa_dout 才是字 0。
            S_LOAD_PRE: state <= S_LOAD;

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
                        lane_lo <= bufa_dout;
                    end else begin
                        kec_wr_en   <= 1'b1;
                        kec_wr_addr <= cnt[5:1];
                        kec_wr_data <= {bufa_dout, lane_lo};
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
                    // ntt_core 的系数存储是 BRAM，读口有一拍延迟：地址 0 这一拍
                    // 发出去，下一拍 rd_data 才是 mem[0]。多插一个空拍把这一拍等掉，
                    // 之后 S_STORE 里"地址提前一拍"的流水就自然对齐了。
                    // keccak 那边是寄存器阵列的组合读，不需要这一拍。
                    state       <= is_ntt ? S_NTT_PRE : S_STORE;
                end
            end

            S_NTT_PRE: begin
                ntt_rd_addr <= 8'd1;
                state       <= S_STORE;
            end

            S_STORE: begin
                if (is_ntt) begin
                    // 同步读要**提前两拍**给地址：地址寄存器本身占一拍
                    // （本拍写的值下一拍才出现在核的 rd_addr 上），
                    // BRAM 的输出寄存器再占一拍。所以 cnt = c 这一拍拿到的是
                    // mem[c]，而寄存器里已经填到 c+2 了。
                    ntt_rd_addr <= cnt[7:0] + 8'd2;
                    // 奇数拍凑齐一个字，由 A 口 mux 写回，这里只存低半字
                    if (!cnt[0]) begin
                        lo_lat <= ntt_rd_data;
                    end
                    if (cnt == 10'd255) begin
                        out_len_r <= 32'd512;
                        out_words <= 9'd128;
                        state     <= S_FIN;
                    end else begin
                        cnt <= cnt + 10'd1;
                    end
                end else begin
                    kec_rd_addr <= cnt[5:1] + {4'd0, cnt[0]};
                    // 结果字由 A 口 mux 写回
                    if (cnt == 10'd49) begin
                        out_len_r <= 32'd200;
                        out_words <= 9'd50;
                        state     <= S_FIN;
                    end else begin
                        cnt <= cnt + 10'd1;
                    end
                end
            end

            // ---- 模式 10：SHAKE / SHA3 ----
            // 与 NTT/Keccak 那两条路的分工不同：这里不"装载→踢→等→回写"，
            // 而是把缓冲区当成一条字节流喂给海绵、再把挤出来的字节写回同一块
            // 缓冲区。中间状态一次也不经过总线。
            S_SHK_KICK: begin
                shk_start <= 1'b1;
                cnt       <= 10'd0;
                state     <= S_SHK_ABS;
            end

            // 吸收。shk_in_valid 是组合的（state 与 cnt 决定），核心的
            // in_ready 在清空海绵和置换期间为低，握上了才推进 cnt。
            // 空消息（msglen==0）直接落到 S_SHK_FLUSH —— SHA3-256("") 是
            // FIPS 202 的第一条向量，不是边角料。
            S_SHK_ABS: begin
                if (cnt == shk_msglen) begin
                    state <= S_SHK_FLUSH;
                end else if (shk_in_ready) begin
                    cnt <= cnt + 10'd1;
                end
            end

            // 宣告消息结束。in_flush 只在 in_valid 为低时被采样，而此刻
            // shk_in_valid 已经为低（cnt == msglen），所以只要等 in_ready。
            // shk_flush 是寄存的，下一拍才到核心 —— 那时本状态机已经在
            // S_SHK_SQ，in_valid 依旧为低，核心仍停在吸收态，采样点成立。
            S_SHK_FLUSH: begin
                if (shk_in_ready) begin
                    shk_flush <= 1'b1;
                    cnt       <= 10'd0;
                    state     <= S_SHK_SQ;
                end
            end

            // 挤压。结果按字节攒进 sq_acc，凑满一个 32 位字才整字写回缓冲区；
            // 消息此时已经全部吸收完，覆盖掉它是安全的，也顺带把明文消息
            // 从缓冲区里抹掉。
            S_SHK_SQ: begin
                if (cnt == shk_outlen) begin
                    out_len_r <= {22'd0, shk_outlen};
                    // 向上取整到字：AXI4-Stream 按 32 位搬运，最后一个字里
                    // 多出来的字节由软件按 OUT_LEN 截断
                    out_words <= {shk_outw[8:0]};
                    shk_zeroize <= 1'b1;
                    state       <= S_SHK_WIPE;
                end else if (shk_out_valid) begin
                    // 字节攒进 sq_acc；攒满一个字（或到输出末尾）时
                    // 由 A 口 mux 整字写回，见上面的 sq_flush。
                    sq_acc <= sq_next;
                    cnt    <= cnt + 10'd1;
                end
            end

            // 等海绵擦完再报 DONE。
            //
            // 不擦是不行的：挤压没有自然终点，核心挤完最后一块会一直停在
            // 挤压态，既不回空闲、也就不肯把置换核借出去 —— 紧接着的一条
            // 模式 9 命令会永远等不到 done。擦除同时把上一条消息的海绵状态
            // 清掉，两件事一次做完。
            //
            // 必须等 busy 落下才报 DONE，否则软件看到 DONE 立刻发下一条
            // 模式 9 命令时，海绵还在写那 25 个 0，会把刚装进去的 lane 冲掉。
            S_SHK_WIPE: begin
                if (!shk_busy) begin
                    state <= S_FIN;
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
