// sym_axi —— AES / SM4 / SM3 三个核的 AXI4-Lite 从机
//
//      s_axi ──▶ axi4lite_firewall ──▶ 寄存器组 ──┬─▶ aes_core
//                 AxPROT/窗口/tamper              ├─▶ sm4_core
//                                                 └─▶ sm3_core
//                                                        ▲
//      key_vault 的 use 口 ────────────────────────────────┘
//                （256 位密钥，**不经过总线**）
//
// ============================================================================
// 【这个模块存在的全部意义：密钥不经过总线】
// ============================================================================
// 常见做法是让软件把密钥写进一组 KEY 寄存器，加密核再从那里读。那样密钥就
// 在总线上走过一遍，也在寄存器里躺着 —— 谁能读那段地址，谁就有密钥。
//
// 这里没有 KEY 寄存器。软件能写的只有 **KEY_SLOT**（一个 3 位的槽号），
// 密钥本体从 `key_vault` 的 use 口直接进算法核，那 256 根线不出 PL。
// 软件说的是"用第 3 号槽的那把钥匙"，而不是"这把钥匙是 0x...."。
//
// 于是"读密钥"这件事在总线上**没有对应的地址**，而不是"有地址但被门控了"。
// 与 key_vault 里那条不变量是同一条，只是延伸到了使用侧。
//
// ============================================================================
// 【寄存器表】（偏移，宽度均 32 位，窗口 0x00~0x7F）
//   0x00 VERSION   R
//   0x04 CTRL      W   [0]=ZEROIZE（写 1 触发，自清；三个核一起擦）
//   0x08 STATUS    R   [0]=BUSY [1]=DONE [2]=KEY_READY [3]=KV_VALID
//   0x0C ALG       RW  [1:0] 0=AES-128 1=AES-256 2=SM4 3=SM3；[2]=DECRYPT
//   0x10 KEY_SLOT  RW  [2:0] 用密钥仓的哪个槽
//   0x14 CMD       W   [0]=LOAD_KEY [1]=BLOCK [2]=HASH_START [3]=HASH_FINAL
//   0x18 HASH_IN   W   [7:0] 送一个字节进 SM3
//   0x1C VIOL_CNT  R   {读违规[31:16], 写违规[15:0]}
//   0x20~0x2C DIN0..3   W   输入分组，DIN0 是最高 32 位
//   0x30~0x3C DOUT0..3  R   输出分组
//   0x40~0x5C DIGEST0..7 R  SM3 摘要
//   0x60 PARAM0    R   {0, 0, 0, 支持的算法位图}
//
// **这张表里没有 KEY 寄存器。** 有意的，见上。
`default_nettype none

module sym_axi #(
    parameter [31:0]  VERSION     = 32'h0001_0000,
    parameter integer SECURE_ONLY = 1
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

    input  wire        tamper,

    // ---- 密钥仓的使用口：这 256 根线不出 PL ----
    output wire [2:0]   kv_sel,
    input  wire [255:0] kv_key,
    input  wire         kv_valid
);
    localparam [1:0] RESP_OKAY = 2'b00;

    localparam [3:0] A_VERSION = 4'h0, A_CTRL = 4'h1, A_STATUS = 4'h2,
                     A_ALG     = 4'h3, A_SLOT = 4'h4, A_CMD    = 4'h5,
                     A_HASHIN  = 4'h6, A_VIOL = 4'h7;
    // 0x20~0x2C → araddr[6:2] = 8..11；0x30~0x3C → 12..15；0x40~0x5C → 16..23

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
    wire        fw_tampered;

    axi4lite_firewall #(
        .AW(8), .SECURE_ONLY(SECURE_ONLY), .PRIV_ONLY(0),
        .ALLOW_WRITE(1), .ALLOW_READ(1),
        .ADDR_BASE(32'h0000_0000), .ADDR_MASK(32'h0000_0080)   // 窗口 0x00~0x7F
    ) u_fw (
        .clk(clk), .rst_n(rst_n), .tamper(tamper),
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
        .viol_first_valid(), .viol_first_addr(), .viol_first_prot(),
        .viol_first_is_write(), .tamper_latched(fw_tampered));

    // ================= 控制寄存器 =================
    reg [1:0]   alg;                 // 0=AES-128 1=AES-256 2=SM4 3=SM3
    reg         dec_flag;
    reg [2:0]   slot;
    reg [127:0] din;
    reg         zero_pulse;
    reg         c_loadkey, c_block, c_hstart, c_hfinal, c_hbyte;
    reg [7:0]   hash_byte;

    assign kv_sel = slot;

    localparam [1:0] ALG_AES128 = 2'd0, ALG_AES256 = 2'd1,
                     ALG_SM4    = 2'd2, ALG_SM3    = 2'd3;

    wire is_aes = (alg == ALG_AES128) || (alg == ALG_AES256);
    wire is_sm4 = (alg == ALG_SM4);
    wire is_sm3 = (alg == ALG_SM3);

    // 擦除：软件的 CTRL.ZEROIZE 或硬件 tamper，两条都直通三个核
    wire zeroize_all = zero_pulse || tamper || fw_tampered;

    // ================= 三个算法核 =================
    // ⚠️ 密钥来自 kv_key，**不是来自任何寄存器**。装载还要 kv_valid 为真 ——
    //    空槽装不进去，免得拿一把全零密钥去加密还以为成功了。
    wire kload = c_loadkey && kv_valid;

    wire         aes_kready, aes_bdone;
    wire [127:0] aes_out;
    aes_core u_aes (
        .clk(clk), .rst_n(rst_n),
        .key_start(kload && is_aes), .key_in(kv_key),
        .key_256(alg == ALG_AES256), .key_ready(aes_kready),
        .blk_start(c_block && is_aes), .decrypt(dec_flag),
        .block_in(din), .block_out(aes_out), .blk_done(aes_bdone),
        .zeroize(zeroize_all));

    wire         sm4_kready, sm4_bdone;
    wire [127:0] sm4_out;
    sm4_core u_sm4 (
        .clk(clk), .rst_n(rst_n),
        .key_start(kload && is_sm4), .key_in(kv_key[255:128]),
        .key_ready(sm4_kready),
        .blk_start(c_block && is_sm4), .decrypt(dec_flag),
        .block_in(din), .block_out(sm4_out), .blk_done(sm4_bdone),
        .zeroize(zeroize_all));

    wire         sm3_done, sm3_in_ready;
    wire [255:0] sm3_digest;
    sm3_core u_sm3 (
        .clk(clk), .rst_n(rst_n),
        .start(c_hstart), .in_valid(c_hbyte), .in_ready(sm3_in_ready),
        .in_data(hash_byte), .in_flush(c_hfinal),
        .done(sm3_done), .digest(sm3_digest),
        .zeroize(zeroize_all));

    wire key_ready = is_sm4 ? sm4_kready : (is_aes ? aes_kready : 1'b0);
    wire op_done   = is_sm3 ? sm3_done   : (is_sm4 ? sm4_bdone : aes_bdone);
    wire [127:0] dout = is_sm4 ? sm4_out : aes_out;

    // ================= 写通道（防火墙下游）=================
    reg aw_got, w_got;
    reg [7:0]  aw_addr_r;
    reg [31:0] w_data_r;
    reg [3:0]  w_strb_r;

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
            alg <= 2'd0; dec_flag <= 1'b0; slot <= 3'd0; din <= 128'd0;
            zero_pulse <= 1'b0; hash_byte <= 8'd0;
            c_loadkey <= 1'b0; c_block <= 1'b0;
            c_hstart <= 1'b0; c_hfinal <= 1'b0; c_hbyte <= 1'b0;
        end else begin
            zero_pulse <= 1'b0;
            c_loadkey  <= 1'b0;
            c_block    <= 1'b0;
            c_hstart   <= 1'b0;
            // HASH_FINAL 与 HASH_IN 是**电平**不是脉冲：sm3_core 的 in_flush /
            // in_valid 只在 in_ready 为高时才被采样，而海绵在压缩时会拉低
            // in_ready 几十拍。写成一拍的脉冲就会丢字节。
            if (sm3_in_ready) begin
                c_hfinal <= 1'b0;
                c_hbyte  <= 1'b0;
            end

            if (f_awvalid && f_awready) begin aw_got <= 1'b1; aw_addr_r <= f_awaddr; end
            if (f_wvalid  && f_wready)  begin w_got  <= 1'b1; w_data_r  <= f_wdata;
                                              w_strb_r <= f_wstrb; end

            if (wr_now) begin
                aw_got <= 1'b0; w_got <= 1'b0;
                f_bvalid <= 1'b1;
                f_bresp  <= RESP_OKAY;

                if (wr_strb[0]) begin
                    if (wr_addr[6:2] >= 5'd8 && wr_addr[6:2] <= 5'd11) begin
                        // DIN0..3：DIN0 是最高 32 位
                        din[127 - 32*(wr_addr[3:2]) -: 32] <= wr_data;
                    end else begin
                        case (wr_addr[5:2])
                        A_CTRL:   zero_pulse <= wr_data[0];
                        A_ALG: begin
                            alg      <= wr_data[1:0];
                            dec_flag <= wr_data[2];
                        end
                        A_SLOT:   slot <= wr_data[2:0];
                        A_CMD: begin
                            c_loadkey <= wr_data[0];
                            c_block   <= wr_data[1];
                            c_hstart  <= wr_data[2];
                            if (wr_data[3]) c_hfinal <= 1'b1;
                        end
                        A_HASHIN: begin
                            hash_byte <= wr_data[7:0];
                            c_hbyte   <= 1'b1;
                        end
                        default: ;
                        endcase
                    end
                end
            end

            if (f_bvalid && f_bready) f_bvalid <= 1'b0;
        end
    end

    // ================= 读通道（防火墙下游）=================
    // ⚠️ kv_key 不出现在这个多路选择器里，也不出现在任何能到达 f_rdata 的
    //    表达式中。test_sym_vault 逐字扫描整个窗口验证这一条。
    assign f_arready = !f_rvalid;

    wire busy = !op_done;
    wire [31:0] r_status = {28'd0, kv_valid, key_ready, op_done, busy};

    reg [31:0] rmux;
    always @(*) begin
        if (f_araddr[6:2] >= 5'd12 && f_araddr[6:2] <= 5'd15)
            rmux = dout[127 - 32*(f_araddr[3:2]) -: 32];          // DOUT0..3
        else if (f_araddr[6:2] >= 5'd16 && f_araddr[6:2] <= 5'd23)
            rmux = sm3_digest[255 - 32*(f_araddr[4:2]) -: 32];    // DIGEST0..7
        else begin
            case (f_araddr[5:2])
            A_VERSION: rmux = VERSION;
            A_STATUS:  rmux = r_status;
            A_ALG:     rmux = {29'd0, dec_flag, alg};
            A_SLOT:    rmux = {29'd0, slot};
            A_VIOL:    rmux = {viol_rd_count, viol_wr_count};
            4'hC:      rmux = 32'h0000_000F;   // PARAM0：四个算法都在
            default:   rmux = 32'd0;
            endcase
        end
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            f_rvalid <= 1'b0; f_rresp <= RESP_OKAY; f_rdata <= 32'd0;
        end else begin
            if (f_arvalid && f_arready) begin
                f_rvalid <= 1'b1;
                f_rresp  <= RESP_OKAY;
                f_rdata  <= rmux;
            end
            if (f_rvalid && f_rready) f_rvalid <= 1'b0;
        end
    end

    wire _unused = &{1'b0, f_awprot, f_arprot, wr_strb[3:1], 1'b0};

endmodule

`default_nettype wire
