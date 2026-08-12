// key_vault_axi —— 密钥仓的 AXI4-Lite 从机（防火墙 + 仓 + 元数据寄存器）
//
//        s_axi ──▶ axi4lite_firewall ──▶ 元数据寄存器组 ──▶ key_vault
//                    (AxPROT/窗口/tamper)                      │
//                                                    use 口 ───┘──▶ PL 内的算法核
//
// 三段的分工是有意分开的：
//   · 防火墙决定"这笔访问该不该存在"，不合规的事务**根本不会出现在下游**；
//   · 寄存器组决定"这笔访问是什么意思"，它只碰元数据；
//   · 仓决定"密钥怎么存"，它的密钥寄存器与总线之间没有导线。
//
// ============================================================================
// 【寄存器表】（偏移，宽度均 32 位）
//   0x00 VERSION    R
//   0x04 CTRL       W   [0]=ZEROIZE（写 1 触发，自清）
//   0x08 STATUS     R   [0]=READY(!tamper)  [1]=TAMPER_LATCHED  [2]=DENY(锁存)
//   0x0C SLOT_SEL   RW  [2:0] 当前操作的槽位
//   0x10 KEY_IN     W   写入一个 32 位字，自动推进字下标。**读恒为 0**
//   0x14 SLOT_CTRL  W   [0]=BEGIN [1]=COMMIT [2]=LOCK [3]=ERASE（写 1 触发）
//   0x18 SLOT_STAT  R   [0]=VALID [1]=LOCKED [7:4]=已写入字数
//   0x1C VALID_MAP  R   每槽一位
//   0x20 LOCK_MAP   R   每槽一位
//   0x24 ZERO_CNT   R   [7:0] 擦除发生过几次
//   0x28 VIOL_CNT   R   {读违规[31:16], 写违规[15:0]}
//   0x2C VIOL_INFO  R   [7:0]=首次违规地址 [8]=是写 [9]=NS 位 [10]=有效
//   0x30 PARAM0     R   {WORDS[15:8], SLOTS[7:0]}
//
// **这张表里没有一个地址会返回密钥材料。** KEY_IN 是只写的，读它返回 0；
// 其余全是元数据。test_key_vault 里有一条用例把 256 字节地址空间**逐字扫一遍**，
// 断言写进去的密钥字一个都不出现 —— 这条不是靠读代码保证的。
//
// ============================================================================
// 【为什么 zeroize 只有"写 1 触发"没有"写 0 取消"】
// ============================================================================
// 擦除是不可逆的、且永远是安全的方向。给它一个"取消"意味着存在一个时刻，
// 软件可以撤销一次已经发起的擦除 —— 那正是被攻陷的软件想要的能力。
`default_nettype none

module key_vault_axi #(
    parameter [31:0]  VERSION     = 32'h0001_0000,
    parameter integer SECURE_ONLY = 1,        // non-secure 访问一律 DECERR
    parameter integer SLOTS       = 8,
    parameter integer SLOT_BITS   = 3,
    parameter integer WORDS       = 8         // 每槽 8 个 32 位字 = 256 bit
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
    output wire [1:0]  s_axi_bresp,
    output wire        s_axi_bvalid,
    input  wire        s_axi_bready,

    input  wire [7:0]  s_axi_araddr,
    input  wire [2:0]  s_axi_arprot,
    input  wire        s_axi_arvalid,
    output wire        s_axi_arready,
    output wire [31:0] s_axi_rdata,
    output wire [1:0]  s_axi_rresp,
    output wire        s_axi_rvalid,
    input  wire        s_axi_rready,

    // ---- 篡改检测：机箱开盖、电压/温度越界、JTAG 插入等接这里 ----
    input  wire        tamper,

    // ---- 使用口：只接 PL 内部的算法核，不出芯片边界 ----
    input  wire [SLOT_BITS-1:0]  use_sel,
    output wire [WORDS*32-1:0]   use_key,
    output wire                  use_valid,

    // ---- 观测口（接 ILA / LED）----
    output wire        vault_tampered
);
    localparam [1:0] RESP_OKAY = 2'b00;

    localparam [3:0] A_VERSION   = 4'h0, A_CTRL      = 4'h1, A_STATUS  = 4'h2,
                     A_SLOT_SEL  = 4'h3, A_KEY_IN    = 4'h4, A_SLOT_CT = 4'h5,
                     A_SLOT_STAT = 4'h6, A_VALID_MAP = 4'h7, A_LOCK_MAP = 4'h8,
                     A_ZERO_CNT  = 4'h9, A_VIOL_CNT  = 4'hA, A_VIOL_INFO = 4'hB,
                     A_PARAM0    = 4'hC;

    // ================= 防火墙 =================
    wire [7:0]  f_awaddr;
    wire [2:0]  f_awprot;
    wire        f_awvalid, f_awready;
    wire [31:0] f_wdata;
    wire [3:0]  f_wstrb;
    wire        f_wvalid, f_wready;
    reg  [1:0]  f_bresp;
    reg         f_bvalid;
    wire        f_bready;

    wire [7:0]  f_araddr;
    wire [2:0]  f_arprot;
    wire        f_arvalid, f_arready;
    reg  [31:0] f_rdata;
    reg  [1:0]  f_rresp;
    reg         f_rvalid;
    wire        f_rready;

    wire [15:0] viol_wr_count, viol_rd_count;
    wire        viol_first_valid, viol_first_is_write;
    wire [7:0]  viol_first_addr;
    wire [2:0]  viol_first_prot;
    wire        fw_tampered;

    axi4lite_firewall #(
        .AW(8),
        .SECURE_ONLY(SECURE_ONLY),
        .PRIV_ONLY(0),
        .ALLOW_WRITE(1),
        .ALLOW_READ(1),
        // 地址窗口：本从机只认 0x00~0x3F，越界一律 DECERR。
        // 写死在这里而不是"读回 0"，是因为越界访问本身就是要留痕的事件。
        .ADDR_BASE(32'h0000_0000),
        .ADDR_MASK(32'h0000_00C0)
    ) u_fw (
        .clk(clk), .rst_n(rst_n),
        .tamper(tamper),

        .s_awaddr(s_axi_awaddr), .s_awprot(s_axi_awprot),
        .s_awvalid(s_axi_awvalid), .s_awready(s_axi_awready),
        .s_wdata(s_axi_wdata), .s_wstrb(s_axi_wstrb),
        .s_wvalid(s_axi_wvalid), .s_wready(s_axi_wready),
        .s_bresp(s_axi_bresp), .s_bvalid(s_axi_bvalid), .s_bready(s_axi_bready),
        .s_araddr(s_axi_araddr), .s_arprot(s_axi_arprot),
        .s_arvalid(s_axi_arvalid), .s_arready(s_axi_arready),
        .s_rdata(s_axi_rdata), .s_rresp(s_axi_rresp),
        .s_rvalid(s_axi_rvalid), .s_rready(s_axi_rready),

        .m_awaddr(f_awaddr), .m_awprot(f_awprot),
        .m_awvalid(f_awvalid), .m_awready(f_awready),
        .m_wdata(f_wdata), .m_wstrb(f_wstrb),
        .m_wvalid(f_wvalid), .m_wready(f_wready),
        .m_bresp(f_bresp), .m_bvalid(f_bvalid), .m_bready(f_bready),
        .m_araddr(f_araddr), .m_arprot(f_arprot),
        .m_arvalid(f_arvalid), .m_arready(f_arready),
        .m_rdata(f_rdata), .m_rresp(f_rresp),
        .m_rvalid(f_rvalid), .m_rready(f_rready),

        .viol_wr_count(viol_wr_count), .viol_rd_count(viol_rd_count),
        .viol_first_valid(viol_first_valid),
        .viol_first_addr(viol_first_addr),
        .viol_first_prot(viol_first_prot),
        .viol_first_is_write(viol_first_is_write),
        .tamper_latched(fw_tampered));

    // ================= 密钥仓 =================
    reg [SLOT_BITS-1:0] slot_sel;
    reg                 ld_begin, ld_we, ld_commit, ld_lock, ld_erase, do_zeroize;
    reg [31:0]          ld_wdata;

    wire [SLOTS-1:0] valid_map, lock_map;
    wire [3:0]       sel_fill;
    wire             vault_tamper_latched, vault_deny;
    wire [7:0]       zeroize_count;

    key_vault #(.SLOTS(SLOTS), .SLOT_BITS(SLOT_BITS), .WORDS(WORDS)) u_vault (
        .clk(clk), .rst_n(rst_n),
        .ld_slot(slot_sel),
        .ld_begin(ld_begin), .ld_we(ld_we), .ld_wdata(ld_wdata),
        .ld_commit(ld_commit), .ld_lock(ld_lock), .ld_erase(ld_erase),
        .zeroize(do_zeroize), .tamper(tamper),
        .use_sel(use_sel), .use_key(use_key), .use_valid(use_valid),
        .valid_map(valid_map), .lock_map(lock_map), .sel_fill(sel_fill),
        .tamper_latched(vault_tamper_latched),
        .zeroize_count(zeroize_count), .deny(vault_deny));

    assign vault_tampered = vault_tamper_latched;

    // ================= 写通道（防火墙下游）=================
    // 到这里的每一笔写都已经被放行过了，所以这一段不必再判权限 ——
    // 这正是把防火墙独立出来的意义：从机里没有"忘了判"的可能。
    reg aw_got, w_got;
    reg [7:0]  aw_addr_r;
    reg [31:0] w_data_r;
    reg [3:0]  w_strb_r;
    reg        deny_sticky;

    assign f_awready = !aw_got && !f_bvalid;
    assign f_wready  = !w_got  && !f_bvalid;

    wire wr_now = (aw_got || (f_awvalid && f_awready))
                  && (w_got || (f_wvalid && f_wready)) && !f_bvalid;
    wire [7:0]  wr_addr = (f_awvalid && f_awready) ? f_awaddr : aw_addr_r;
    wire [31:0] wr_data = (f_wvalid  && f_wready)  ? f_wdata  : w_data_r;
    wire [3:0]  wr_strb = (f_wvalid  && f_wready)  ? f_wstrb  : w_strb_r;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            aw_got <= 1'b0; w_got <= 1'b0;
            aw_addr_r <= 8'd0; w_data_r <= 32'd0; w_strb_r <= 4'd0;
            f_bvalid <= 1'b0; f_bresp <= RESP_OKAY;
            slot_sel <= {SLOT_BITS{1'b0}};
            ld_begin <= 1'b0; ld_we <= 1'b0; ld_commit <= 1'b0;
            ld_lock <= 1'b0; ld_erase <= 1'b0; do_zeroize <= 1'b0;
            ld_wdata <= 32'd0;
            deny_sticky <= 1'b0;
        end else begin
            ld_begin <= 1'b0; ld_we <= 1'b0; ld_commit <= 1'b0;
            ld_lock <= 1'b0; ld_erase <= 1'b0; do_zeroize <= 1'b0;

            if (vault_deny) deny_sticky <= 1'b1;

            if (f_awvalid && f_awready) begin aw_got <= 1'b1; aw_addr_r <= f_awaddr; end
            if (f_wvalid  && f_wready)  begin w_got  <= 1'b1; w_data_r  <= f_wdata;
                                              w_strb_r <= f_wstrb; end

            if (wr_now) begin
                aw_got <= 1'b0; w_got <= 1'b0;
                f_bvalid <= 1'b1;
                f_bresp  <= RESP_OKAY;

                if (wr_strb[0]) begin
                    case (wr_addr[5:2])
                    A_CTRL: begin
                        do_zeroize  <= wr_data[0];
                        // 擦除同时清掉 DENY 的锁存：新的一轮从干净状态开始
                        if (wr_data[0]) deny_sticky <= 1'b0;
                    end
                    A_SLOT_SEL: slot_sel <= wr_data[SLOT_BITS-1:0];
                    A_KEY_IN: begin
                        ld_we    <= 1'b1;
                        ld_wdata <= wr_data;
                    end
                    A_SLOT_CT: begin
                        // 一次只让一个动作生效，优先级从"最无害"到"最有影响"。
                        // 同时写多位是调用方的错，但不该变成未定义行为。
                        ld_begin  <= wr_data[0];
                        ld_commit <= wr_data[1] && !wr_data[0];
                        ld_lock   <= wr_data[2] && !wr_data[0] && !wr_data[1];
                        ld_erase  <= wr_data[3] && !wr_data[0] && !wr_data[1]
                                                && !wr_data[2];
                    end
                    default: ;
                    endcase
                end
            end

            if (f_bvalid && f_bready) f_bvalid <= 1'b0;
        end
    end

    // ================= 读通道（防火墙下游）=================
    // ⚠️ 这个多路选择器里**只有元数据**。keys 阵列不出现在这里，
    //    也不出现在任何能到达 f_rdata 的表达式中。
    assign f_arready = !f_rvalid;

    wire [31:0] r_status = {29'd0, deny_sticky, vault_tamper_latched,
                            !vault_tamper_latched};
    wire [31:0] r_slot_stat = {24'd0, sel_fill,
                               2'd0, lock_map[slot_sel], valid_map[slot_sel]};
    wire [31:0] r_viol_info = {21'd0, viol_first_valid, viol_first_prot[1],
                               viol_first_is_write, viol_first_addr};

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            f_rvalid <= 1'b0;
            f_rresp  <= RESP_OKAY;
            f_rdata  <= 32'd0;
        end else begin
            if (f_arvalid && f_arready) begin
                f_rvalid <= 1'b1;
                f_rresp  <= RESP_OKAY;
                case (f_araddr[5:2])
                A_VERSION:   f_rdata <= VERSION;
                A_STATUS:    f_rdata <= r_status;
                A_SLOT_SEL:  f_rdata <= {{(32-SLOT_BITS){1'b0}}, slot_sel};
                // KEY_IN 是只写口。读它返回 0 —— 这不是"门控成 0"，
                // 是这一支本来就没有连到密钥寄存器。
                A_KEY_IN:    f_rdata <= 32'd0;
                A_SLOT_STAT: f_rdata <= r_slot_stat;
                A_VALID_MAP: f_rdata <= {{(32-SLOTS){1'b0}}, valid_map};
                A_LOCK_MAP:  f_rdata <= {{(32-SLOTS){1'b0}}, lock_map};
                A_ZERO_CNT:  f_rdata <= {24'd0, zeroize_count};
                A_VIOL_CNT:  f_rdata <= {viol_rd_count, viol_wr_count};
                A_VIOL_INFO: f_rdata <= r_viol_info;
                A_PARAM0:    f_rdata <= {16'd0, WORDS[7:0], SLOTS[7:0]};
                // CTRL 与 SLOT_CTRL 是只写的；读它们返回 0
                default:     f_rdata <= 32'd0;
                endcase
            end
            if (f_rvalid && f_rready) f_rvalid <= 1'b0;
        end
    end

    wire _unused = &{1'b0, f_awprot, f_arprot, wr_strb[3:1], fw_tampered, 1'b0};

endmodule

`default_nettype wire
