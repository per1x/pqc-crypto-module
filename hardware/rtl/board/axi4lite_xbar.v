// axi4lite_xbar —— 一主多从的 AXI4-Lite 地址译码
//
// PS 的 M_AXI_HPM0_LPD 是**一个**主口，而 PL 里有五个从机。这一层按地址高位
// 把事务路到其中一个，命不中任何一个就地回 DECERR。
//
// ============================================================================
// 【为什么自己写，不用 Xilinx 的 AXI Interconnect / SmartConnect】
// ============================================================================
// 那两个 IP 解决的是"多主、跨时钟域、位宽转换、乱序、突发"这些问题 ——
// 本设计一个都没有：单主、单时钟、全是 32 位 AXI4-Lite、每次一笔。
// 引进来的话会带一堆用不上的逻辑，更要紧的是**它不进 cocotb 回归**
// （加密 IP 不能在 Icarus 里仿），于是地址译码这段就成了整条链上唯一
// 没被对拍覆盖的地方。自己写的这一段和别的模块一样有用例顶着。
//
// ============================================================================
// 【地址划分】每个从机 64 KB 一个窗口，够用且好记
// ============================================================================
//   槽 0  +0x0_0000  trng_axi
//   槽 1  +0x1_0000  key_vault_axi
//   槽 2  +0x2_0000  sym_axi        （AES / SM4 / SM3）
//   槽 3  +0x3_0000  mlkem_axi
//   槽 4  +0x4_0000  金丝雀（SECURE_ONLY=1，只用来被拒）
//   槽 5  +0x5_0000  fan_ctrl_axi   （风扇观测口）
//
// ============================================================================
// 【完整译码，不留镜像地址】
// ============================================================================
// 第一版只用 addr[SEL_LSB +: 3] 选槽、只把 addr[7:0] 交给从机，中间的
// addr[15:8] 与 aperture 以上的高位**整个被丢掉**。后果是每个寄存器都有
// 大量镜像：0x8001_0000、0x8001_0100、0x8001_1100 …… 落到同一个寄存器上。
//
// 当时的理由是"窗口检查是从机的策略，放这里会变成两处策略"。这个理由站不住：
// 从机根本收不到被丢掉的那些位，**它没有能力判自己的窗口**——策略不是被下放了，
// 是没有人执行。而镜像地址是实打实的问题：
//
//   · PS 侧要用 XMPU/XPPU 按地址段限制访问时，被限制的那一段总能从某个
//     镜像地址绕过去 —— 保护的是地址，不是寄存器；
//   · "地址映射"这份文档从此不可能是准确的：真正可达的地址集合比表里列的大
//     好几个数量级，任何按表做的审计都会漏。
//
// 所以译码在这里做全：
//   ① aperture：addr 的高位必须与 BASE_ADDR 完全一致（落在 NS 个槽的范围内）；
//   ② 槽号必须小于 NS；
//   ③ 槽内偏移的高位必须全 0 —— 从机的寄存器表只有 OFF_BITS 位，
//      超出的部分不是"从机的事"，而是根本没有这个地址；
//   ④ 32 位对齐。不对齐的地址在旧版里也是镜像（0x05 读到 0x04 那个寄存器）。
// 任何一条不成立就**就地 DECERR，事务不往下游发**。
//
// 于是地址映射变成一一对应：一个寄存器有且只有一个地址能访问到它。
// 从机自己的防火墙仍然保留窗口检查（axi4lite_firewall 的 ADDR_MASK）——
// 两道都在，是纵深，不是重复；而且从机现在拿到的地址确实经过了这里的筛选。
//
// 单笔在途：一次只处理一个读事务和一个写事务。控制总线上这样够了，
// 而且**"一次只有一笔"本身就消掉了乱序与响应错配这一整类 bug**。
`default_nettype none

module axi4lite_xbar #(
    parameter integer AW       = 32,   // 上游地址位宽
    parameter integer NS       = 5,    // 从机个数
    parameter integer SEL_LSB  = 16,   // 槽号取 addr[SEL_LSB +: 3]
    parameter integer OFF_BITS = 8,    // 下游从机的地址位宽（槽内偏移）
    // aperture 基址。高于 SEL_LSB+3 的位必须与它逐位相同，否则不命中。
    parameter [63:0]  BASE_ADDR = 64'h0000_0000_8000_0000
) (
    input  wire            clk,
    input  wire            rst_n,

    // ---- 上游（面向 PS）----
    input  wire [AW-1:0]   s_awaddr,
    input  wire [2:0]      s_awprot,
    input  wire            s_awvalid,
    output wire            s_awready,
    input  wire [31:0]     s_wdata,
    input  wire [3:0]      s_wstrb,
    input  wire            s_wvalid,
    output wire            s_wready,
    output reg  [1:0]      s_bresp,
    output reg             s_bvalid,
    input  wire            s_bready,

    input  wire [AW-1:0]   s_araddr,
    input  wire [2:0]      s_arprot,
    input  wire            s_arvalid,
    output wire            s_arready,
    output reg  [31:0]     s_rdata,
    output reg  [1:0]      s_rresp,
    output reg             s_rvalid,
    input  wire            s_rready,

    // ---- 下游：NS 个从机，扁平打包 ----
    output wire [OFF_BITS*NS-1:0] m_awaddr,
    output wire [3*NS-1:0]  m_awprot,
    output reg  [NS-1:0]    m_awvalid,
    input  wire [NS-1:0]    m_awready,
    output wire [32*NS-1:0] m_wdata,
    output wire [4*NS-1:0]  m_wstrb,
    output reg  [NS-1:0]    m_wvalid,
    input  wire [NS-1:0]    m_wready,
    input  wire [2*NS-1:0]  m_bresp,
    input  wire [NS-1:0]    m_bvalid,
    output reg  [NS-1:0]    m_bready,

    output wire [OFF_BITS*NS-1:0] m_araddr,
    output wire [3*NS-1:0]  m_arprot,
    output reg  [NS-1:0]    m_arvalid,
    input  wire [NS-1:0]    m_arready,
    input  wire [32*NS-1:0] m_rdata,
    input  wire [2*NS-1:0]  m_rresp,
    input  wire [NS-1:0]    m_rvalid,
    output reg  [NS-1:0]    m_rready
);
    localparam [1:0] RESP_OKAY = 2'b00, RESP_DECERR = 2'b11;

    // 所有从机拿到同一份地址/数据，靠 valid 选中谁 —— 省掉 NS 份多路选择器
    reg  [OFF_BITS-1:0] aw_addr_r, ar_addr_r;
    reg  [2:0]  aw_prot_r, ar_prot_r;
    reg  [31:0] w_data_r;
    reg  [3:0]  w_strb_r;
    reg  [2:0]  aw_sel, ar_sel;
    reg         aw_hit, ar_hit;

    genvar gi;
    generate
        for (gi = 0; gi < NS; gi = gi + 1) begin : g_fan
            assign m_awaddr[OFF_BITS*gi +: OFF_BITS] = aw_addr_r;
            assign m_awprot[3*gi +: 3]  = aw_prot_r;
            assign m_wdata [32*gi +: 32] = w_data_r;
            assign m_wstrb [4*gi +: 4]  = w_strb_r;
            assign m_araddr[OFF_BITS*gi +: OFF_BITS] = ar_addr_r;
            assign m_arprot[3*gi +: 3]  = ar_prot_r;
        end
    endgenerate

    function automatic [2:0] slot_of;
        input [AW-1:0] a;
        begin
            slot_of = a[SEL_LSB +: 3];
        end
    endfunction

    // 完整译码。四条全部成立才算命中；任何一条不成立都是"没有这个地址"，
    // 就地 DECERR，下游一个字节都收不到。
    //
    // ⚠️ 这个函数是本模块唯一的命中判据 —— 写、读两条通道都调它。
    //    历史上写通道有过"锁存值还没更新"的坑，所以下面每处都是
    //    「本拍握手就用组合地址，否则用锁存值」，两条通道判据必须同源。
    function automatic hit_of;
        input [AW-1:0] a;
        reg ok;
        begin
            ok = 1'b1;
            // ① aperture：槽号以上的高位必须与基址完全一致
            if (a[AW-1 : SEL_LSB+3] != BASE_ADDR[AW-1 : SEL_LSB+3]) ok = 1'b0;
            // ② 槽号必须真的存在（NS 可能不是 8，所以按 32 位比）
            if ({29'd0, a[SEL_LSB +: 3]} >= NS[31:0])               ok = 1'b0;
            // ③ 槽内偏移的高位必须全 0 —— 否则就是一个镜像地址
            if (a[SEL_LSB-1 : OFF_BITS] != {(SEL_LSB-OFF_BITS){1'b0}}) ok = 1'b0;
            // ④ 32 位对齐。不查的话 0x05 会读到 0x04 那个寄存器
            if (a[1:0] != 2'b00)                                    ok = 1'b0;
            hit_of = ok;
        end
    endfunction

    // ================= 写通道 =================
    localparam [2:0] W_IDLE = 3'd0, W_FWD = 3'd1, W_WAITB = 3'd2,
                     W_ERR  = 3'd3, W_RESP = 3'd4;

    reg [2:0] wst;
    reg       aw_got, w_got, aw_sent, w_sent;

    assign s_awready = (wst == W_IDLE) && !aw_got;
    assign s_wready  = (wst == W_IDLE) && !w_got;

    integer i;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            wst <= W_IDLE; aw_got <= 1'b0; w_got <= 1'b0;
            aw_sent <= 1'b0; w_sent <= 1'b0;
            aw_addr_r <= {OFF_BITS{1'b0}}; aw_prot_r <= 3'd0;
            w_data_r <= 32'd0; w_strb_r <= 4'd0;
            aw_sel <= 3'd0; aw_hit <= 1'b0;
            m_awvalid <= {NS{1'b0}}; m_wvalid <= {NS{1'b0}};
            m_bready <= {NS{1'b0}};
            s_bvalid <= 1'b0; s_bresp <= RESP_OKAY;
        end else begin
            case (wst)
            W_IDLE: begin
                if (s_awvalid && s_awready) begin
                    aw_got    <= 1'b1;
                    aw_addr_r <= s_awaddr[OFF_BITS-1:0];
                    aw_prot_r <= s_awprot;
                    aw_sel    <= slot_of(s_awaddr);
                    aw_hit    <= hit_of(s_awaddr);
                end
                if (s_wvalid && s_wready) begin
                    w_got    <= 1'b1;
                    w_data_r <= s_wdata;
                    w_strb_r <= s_wstrb;
                end
                if ((aw_got || (s_awvalid && s_awready))
                    && (w_got || (s_wvalid && s_wready))) begin
                    aw_got <= 1'b0; w_got <= 1'b0;
                    // 这一拍锁存值可能还没更新，用组合值再判一次
                    if (s_awvalid && s_awready) begin
                        aw_addr_r <= s_awaddr[OFF_BITS-1:0];
                        aw_prot_r <= s_awprot;
                        aw_sel    <= slot_of(s_awaddr);
                    end
                    if (s_wvalid && s_wready) begin
                        w_data_r <= s_wdata;
                        w_strb_r <= s_wstrb;
                    end
                    if ((s_awvalid && s_awready) ? hit_of(s_awaddr)
                                                 : aw_hit) begin
                        for (i = 0; i < NS; i = i + 1)
                            if (i[2:0] == ((s_awvalid && s_awready)
                                           ? slot_of(s_awaddr) : aw_sel)) begin
                                m_awvalid[i] <= 1'b1;
                                m_wvalid[i]  <= 1'b1;
                            end
                        aw_sent <= 1'b0; w_sent <= 1'b0;
                        wst     <= W_FWD;
                    end else begin
                        wst <= W_ERR;
                    end
                end
            end

            W_FWD: begin
                if (|(m_awvalid & m_awready)) begin
                    m_awvalid <= {NS{1'b0}}; aw_sent <= 1'b1;
                end
                if (|(m_wvalid & m_wready)) begin
                    m_wvalid <= {NS{1'b0}}; w_sent <= 1'b1;
                end
                if ((aw_sent || |(m_awvalid & m_awready))
                    && (w_sent || |(m_wvalid & m_wready))) begin
                    for (i = 0; i < NS; i = i + 1)
                        if (i[2:0] == aw_sel) m_bready[i] <= 1'b1;
                    wst <= W_WAITB;
                end
            end

            W_WAITB: if (|(m_bvalid & m_bready)) begin
                m_bready <= {NS{1'b0}};
                for (i = 0; i < NS; i = i + 1)
                    if (i[2:0] == aw_sel) s_bresp <= m_bresp[2*i +: 2];
                s_bvalid <= 1'b1;
                wst      <= W_RESP;
            end

            // 没命中任何从机：就地回 DECERR，事务**不往下游发**
            W_ERR: begin
                s_bresp  <= RESP_DECERR;
                s_bvalid <= 1'b1;
                wst      <= W_RESP;
            end

            W_RESP: if (s_bvalid && s_bready) begin
                s_bvalid <= 1'b0;
                wst      <= W_IDLE;
            end

            default: wst <= W_IDLE;
            endcase
        end
    end

    // ================= 读通道 =================
    localparam [2:0] R_IDLE = 3'd0, R_FWD = 3'd1, R_WAITR = 3'd2,
                     R_ERR  = 3'd3, R_RESP = 3'd4;

    reg [2:0] rst_st;

    assign s_arready = (rst_st == R_IDLE);

    integer j;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rst_st <= R_IDLE;
            ar_addr_r <= {OFF_BITS{1'b0}}; ar_prot_r <= 3'd0;
            ar_sel <= 3'd0; ar_hit <= 1'b0;
            m_arvalid <= {NS{1'b0}}; m_rready <= {NS{1'b0}};
            s_rvalid <= 1'b0; s_rresp <= RESP_OKAY; s_rdata <= 32'd0;
        end else begin
            case (rst_st)
            R_IDLE: if (s_arvalid && s_arready) begin
                ar_addr_r <= s_araddr[OFF_BITS-1:0];
                ar_prot_r <= s_arprot;
                ar_sel    <= slot_of(s_araddr);
                ar_hit    <= hit_of(s_araddr);
                if (hit_of(s_araddr)) begin
                    for (j = 0; j < NS; j = j + 1)
                        if (j[2:0] == slot_of(s_araddr)) m_arvalid[j] <= 1'b1;
                    rst_st <= R_FWD;
                end else begin
                    rst_st <= R_ERR;
                end
            end

            R_FWD: if (|(m_arvalid & m_arready)) begin
                m_arvalid <= {NS{1'b0}};
                for (j = 0; j < NS; j = j + 1)
                    if (j[2:0] == ar_sel) m_rready[j] <= 1'b1;
                rst_st <= R_WAITR;
            end

            R_WAITR: if (|(m_rvalid & m_rready)) begin
                m_rready <= {NS{1'b0}};
                for (j = 0; j < NS; j = j + 1)
                    if (j[2:0] == ar_sel) begin
                        s_rdata <= m_rdata[32*j +: 32];
                        s_rresp <= m_rresp[2*j +: 2];
                    end
                s_rvalid <= 1'b1;
                rst_st   <= R_RESP;
            end

            // 没命中：回 0 + DECERR，且**没有任何从机被访问过**
            R_ERR: begin
                s_rdata  <= 32'd0;
                s_rresp  <= RESP_DECERR;
                s_rvalid <= 1'b1;
                rst_st   <= R_RESP;
            end

            R_RESP: if (s_rvalid && s_rready) begin
                s_rvalid <= 1'b0;
                rst_st   <= R_IDLE;
            end

            default: rst_st <= R_IDLE;
            endcase
        end
    end

    // 地址的每一位现在都参与了译码，没有“未使用”的地址位了。
    wire _unused = &{1'b0, ar_hit, 1'b0};

endmodule

`default_nettype wire
