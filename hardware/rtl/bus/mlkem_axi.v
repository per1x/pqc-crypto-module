// mlkem_axi —— ML-KEM 三个整核（KeyGen / Encaps / Decaps）的 AXI4-Lite 从机
//
//   s_axi ──▶ axi4lite_firewall ──▶ 寄存器组 ──┬─▶ mlkem_keygen
//              AxPROT/窗口/tamper              ├─▶ mlkem_encaps
//                                              └─▶ mlkem_decaps
//                    输入缓冲(8 KB) ──喂──┘   └──收──▶ 输出缓冲(8 KB)
//
// ============================================================================
// 【为什么所有输入都从同一个字节缓冲走】
// ============================================================================
// 三个核的输入形状完全不同：KeyGen 要两个 256 位的并行种子，Encaps 要一个
// 256 位的 m 加一条 ek 字节流，Decaps 要两条字节流（dk 与 c）。给每一种开一
// 组寄存器的话，寄存器表会长成三份互不相干的东西，软件侧也要写三套搬运。
//
// 这里统一成"**一切都往 IN_DATA 里灌，顺序就是标准里的顺序**"：
//
//   KeyGen : d(32) ‖ z(32)
//   Encaps : m(32) ‖ ek(384k+32)
//   Decaps : dk(768k+96) ‖ c(32·(du·k+dv))
//
// 需要并行送进核里的那几个 256 位量（d/z/m），由本模块从缓冲区头部读出来
// 装进寄存器 —— 软件不必知道哪些是"流"、哪些是"并行口"。
//
// 长度全部由 param_set 算出来，软件不用报长度，也就不存在"报错长度"这种
// 会安静产生错误结果的输入方式。
//
// ============================================================================
// 【密钥材料的去向，说清楚】
// ============================================================================
// 与 key_vault 那条不变量**不同**：ML-KEM 的 dk 本来就要交给软件（它是这套
// 协议的私钥，密码机把它包装之后存到外面去），所以输出缓冲里确实有私钥字节，
// 也确实能从 OUT_DATA 读出来 —— 这不是漏洞，是接口定义。
//
// 真正的边界在于：
//   · 中间量（ŝ、ê、Â、r̂、m′、重加密出来的 c′）**一个都不进缓冲区**，
//     它们只在核内部的 BRAM 里存在过；
//   · DEBUG_BANK 在这里恒为 0，多项式存储的读口根本没有引出来；
//   · tamper 一根线同时打掉三个核与两个缓冲区。
//
// 换句话说，**软件能拿到的只有算法定义里本来就该给它的那些字节**。
`default_nettype none

module mlkem_axi #(
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

    input  wire        tamper
);
    localparam [1:0] RESP_OKAY = 2'b00;

    localparam [3:0] A_VERSION = 4'h0, A_CTRL   = 4'h1, A_STATUS = 4'h2,
                     A_MODE    = 4'h3, A_INDATA = 4'h4, A_INPTR  = 4'h5,
                     A_OUTDATA = 4'h6, A_OUTLEN = 4'h7, A_OUTRD  = 4'h8,
                     A_VIOL    = 4'h9, A_PARAM0 = 4'hA;

    localparam [1:0] M_KEYGEN = 2'd0, M_ENCAPS = 2'd1, M_DECAPS = 2'd2;

    // ================= 防火墙 =================
    wire [7:0]  f_awaddr;  wire [2:0] f_awprot;
    wire        f_awvalid, f_awready;
    wire [31:0] f_wdata;   wire [3:0] f_wstrb;
    wire        f_wvalid,  f_wready;
    reg  [1:0]  f_bresp;   reg        f_bvalid;   wire f_bready;
    wire [7:0]  f_araddr;  wire [2:0] f_arprot;
    wire        f_arvalid, f_arready;
    reg  [31:0] f_rdata;   reg  [1:0] f_rresp;    reg  f_rvalid;  wire f_rready;
    wire [15:0] viol_wr_count, viol_rd_count;
    wire        fw_tampered;

    axi4lite_firewall #(
        .AW(8), .SECURE_ONLY(SECURE_ONLY), .PRIV_ONLY(0),
        .ALLOW_WRITE(1), .ALLOW_READ(1),
        .ADDR_BASE(32'h0000_0000), .ADDR_MASK(32'h0000_00C0)
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

    // ================= 两块字节缓冲 =================
    // 输入最大是 Decaps 的 dk+c = 3168+1568 = 4736；
    // 输出最大是 KeyGen-1024 的 ek+dk = 6208。8 KB 两块都装得下。
    reg         ina_we;   reg [12:0] ina_addr;  reg [7:0] ina_din;
    reg  [12:0] inb_addr; wire [7:0] inb_dout;
    ram_dp #(.DW(8), .AW(13)) u_inbuf (
        .clk(clk),
        .a_we(ina_we), .a_addr(ina_addr), .a_din(ina_din), .a_dout(),
        .b_we(1'b0),   .b_addr(inb_addr), .b_din(8'd0),    .b_dout(inb_dout));

    reg         outa_we;  reg [12:0] outa_addr; reg [7:0] outa_din;
    reg  [12:0] outb_addr; wire [7:0] outb_dout;
    ram_dp #(.DW(8), .AW(13)) u_outbuf (
        .clk(clk),
        .a_we(outa_we), .a_addr(outa_addr), .a_din(outa_din), .a_dout(),
        .b_we(1'b0),    .b_addr(outb_addr), .b_din(8'd0),     .b_dout(outb_dout));

    // ================= 控制寄存器 =================
    reg [1:0]  mode, pset;
    reg [12:0] in_ptr, out_len, out_rd;
    reg        zero_pulse;

    // ---- 由 param_set 算出来的长度（软件不用报，也就报不错）----
    wire [2:0]  k    = (pset == 2'd0) ? 3'd2 : (pset == 2'd1) ? 3'd3 : 3'd4;
    wire        d11  = (pset == 2'd2);
    wire [3:0]  dv   = d11 ? 4'd5 : 4'd4;
    wire [13:0] eklen = {3'b0, k, 8'd0} + {4'b0, k, 7'd0} + 14'd32;   // 384k+32
    wire [13:0] dklen = {2'b0, k, 9'd0} + {3'b0, k, 8'd0} + 14'd96;   // 768k+96
    wire [13:0] c1len = {3'b0, k, 8'd0} + {5'b0, k, 6'd0}
                        + (d11 ? {6'b0, k, 5'd0} : 14'd0);
    wire [13:0] clen  = c1len + {5'b0, dv, 5'd0};

    // ================= 状态机 =================
    localparam [2:0] S_IDLE = 3'd0, S_PRE = 3'd1, S_KICK = 3'd2,
                     S_RUN  = 3'd3, S_FIN = 3'd4;
    reg [2:0]  state;
    reg [6:0]  pre_cnt;                 // 预读 d/z/m 的字节计数
    reg [255:0] seed_a, seed_b;         // KeyGen 的 d/z；Encaps 的 m 放 seed_a
    reg [12:0] fp;                      // 输入流的读指针
    reg [7:0]  fb_r;                    // 从缓冲取出的一个字节
    reg        fb_v, fb_wait;
    reg        run_done;
    // start 是非阻塞赋值，要到下一拍才真正拉高；而三个核的 done 是**电平**，
    // 一直保持到下一次 start 才清。所以进 S_RUN 的头几拍绝不能看 core_dn ——
    // 看了就会读到上一次运行残留的 done，当场结束、out_len 是 0。
    // 表现：**每个核第一次跑永远对，第二次必错**（真机上就是这么暴露的；
    // 仿真里每个核只跑一次，跑两次那条中间又有 zeroize 复位，所以没抓到）。
    reg [1:0]  kickdly;

    // 本次要往核里喂多少字节（KeyGen 不喂流）
    wire [13:0] feed_len = (mode == M_ENCAPS) ? (14'd32 + eklen)
                         : (mode == M_DECAPS) ? (dklen + clen)
                                              : 14'd0;
    // 预读多少字节进并行寄存器
    wire [6:0]  pre_len  = (mode == M_KEYGEN) ? 7'd64
                         : (mode == M_ENCAPS) ? 7'd32 : 7'd0;

    // ================= 三个核 =================
    // ⚠️ DEBUG_BANK 恒为 0：多项式存储的读口在这里根本没有引出来。
    wire zeroize_all = zero_pulse || tamper || fw_tampered;

    reg  kg_start, en_start, de_start;
    wire kg_done, en_done, de_done, de_hash_ok;
    wire kg_ov, en_ov, de_ov;
    wire kg_ol, en_ol, de_ol;
    wire [7:0] kg_od, en_od, de_od;
    wire en_ekr, de_dkr, de_cr;

    // 输出侧：跑起来之后一直 ready —— 缓冲区总收得下（8 KB 大于任何一次输出）
    wire out_rdy = (state == S_RUN);

    mlkem_keygen #(.DEBUG_BANK(0)) u_kg (
        .clk(clk), .rst_n(rst_n && !zeroize_all),
        .param_set(pset), .d_in(seed_a), .z_in(seed_b),
        .start(kg_start), .done(kg_done),
        .out_valid(kg_ov), .out_ready(out_rdy && (mode == M_KEYGEN)),
        .out_data(kg_od), .out_last(kg_ol),
        .dbg_addr(12'd0), .dbg_data());

    mlkem_encaps #(.DEBUG_BANK(0)) u_en (
        .clk(clk), .rst_n(rst_n && !zeroize_all),
        .param_set(pset), .m_in(seed_a),
        .start(en_start), .done(en_done),
        .ek_valid(fb_v && (mode == M_ENCAPS) && (fp >= 13'd32)),
        .ek_ready(en_ekr), .ek_data(fb_r),
        .out_valid(en_ov), .out_ready(out_rdy && (mode == M_ENCAPS)),
        .out_data(en_od), .out_last(en_ol),
        .dbg_addr(12'd0), .dbg_data());

    mlkem_decaps #(.DEBUG_BANK(0)) u_de (
        .clk(clk), .rst_n(rst_n && !zeroize_all),
        .param_set(pset),
        .start(de_start), .done(de_done),
        .dk_valid(fb_v && (mode == M_DECAPS) && ({1'b0, fp} <  dklen)),
        .dk_ready(de_dkr), .dk_data(fb_r),
        .c_valid (fb_v && (mode == M_DECAPS) && ({1'b0, fp} >= dklen)),
        .c_ready(de_cr), .c_data(fb_r),
        .out_valid(de_ov), .out_ready(out_rdy && (mode == M_DECAPS)),
        .out_data(de_od), .out_last(de_ol),
        .dk_hash_ok(de_hash_ok),
        .dbg_addr(11'd0), .dbg_data());

    // 当前模式下核的输出与"喂进去了没有"
    wire core_ov = (mode == M_KEYGEN) ? kg_ov : (mode == M_ENCAPS) ? en_ov : de_ov;
    wire [7:0] core_od = (mode == M_KEYGEN) ? kg_od
                       : (mode == M_ENCAPS) ? en_od : de_od;
    wire core_ol = (mode == M_KEYGEN) ? kg_ol : (mode == M_ENCAPS) ? en_ol : de_ol;
    wire core_dn = (mode == M_KEYGEN) ? kg_done
                 : (mode == M_ENCAPS) ? en_done : de_done;

    wire feed_fire = (mode == M_ENCAPS) ? (fb_v && (fp >= 13'd32) && en_ekr)
                   : (mode == M_DECAPS) ? (fb_v && (({1'b0, fp} < dklen) ? de_dkr
                                                                        : de_cr))
                                        : 1'b0;

    // ================= 写通道 =================
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

    wire wr_indata = wr_now && wr_strb[0] && (wr_addr[5:2] == A_INDATA)
                     && (state == S_IDLE);

    // ================= 读通道 =================
    assign f_arready = !f_rvalid;
    wire rd_outdata = f_arvalid && f_arready && (f_araddr[5:2] == A_OUTDATA)
                      && ({1'b0, out_rd} < {1'b0, out_len});

    // 四个标志占 [3:0]，填充要 28 位（原来写的 27'd0 让整条拼接只有 31 位，
    // 靠赋值时的零扩展才凑够 32 —— 值不受影响，但位宽是错的）。
    wire [31:0] r_status = {28'd0, fw_tampered, de_hash_ok,
                            (state == S_IDLE) && run_done, (state != S_IDLE)};

    // ================= 端口归属 =================
    always @(*) begin
        ina_we   = wr_indata;
        ina_addr = in_ptr;
        ina_din  = wr_data[7:0];

        // 输入缓冲的读口。
        // ⚠️ 预读阶段地址要**提前一拍**：同步读的数据下一拍才出来，
        //    照着 pre_cnt 给地址的话每个字节会被读两遍、后一半直接丢掉
        //    （表现是 ek 整个不对，而 Decaps 因为不走预读所以照样过）。
        //    喂流阶段不用提前，那里靠 fb_wait 等那一拍。
        inb_addr = (state == S_PRE)
                   ? (fb_wait ? 13'd0 : ({6'd0, pre_cnt} + 13'd1))
                   : fp;

        outa_we   = (state == S_RUN) && core_ov;
        outa_addr = out_len;
        outa_din  = core_od;

        // 同步读要提前一拍：这一拍读命中就把地址推到下一个
        outb_addr = out_rd + {12'd0, rd_outdata};
    end

    // ================= 时序 =================
    integer unused_i;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            aw_got <= 1'b0; w_got <= 1'b0;
            aw_addr_r <= 8'd0; w_data_r <= 32'd0; w_strb_r <= 4'd0;
            f_bvalid <= 1'b0; f_bresp <= RESP_OKAY;
            f_rvalid <= 1'b0; f_rresp <= RESP_OKAY; f_rdata <= 32'd0;
            mode <= 2'd0; pset <= 2'd1;
            in_ptr <= 13'd0; out_len <= 13'd0; out_rd <= 13'd0;
            zero_pulse <= 1'b0;
            state <= S_IDLE; pre_cnt <= 7'd0; fp <= 13'd0; kickdly <= 2'd0;
            seed_a <= 256'd0; seed_b <= 256'd0;
            fb_r <= 8'd0; fb_v <= 1'b0; fb_wait <= 1'b0;
            kg_start <= 1'b0; en_start <= 1'b0; de_start <= 1'b0;
            run_done <= 1'b0;
        end else begin
            zero_pulse <= 1'b0;
            kg_start <= 1'b0; en_start <= 1'b0; de_start <= 1'b0;

            if (zeroize_all) begin
                // 缓冲区里有私钥字节，擦除要连它们一起 —— 指针清零之后
                // OUT_DATA 读不出任何东西（out_len = 0）
                in_ptr <= 13'd0; out_len <= 13'd0; out_rd <= 13'd0;
                seed_a <= 256'd0; seed_b <= 256'd0;
                state  <= S_IDLE; run_done <= 1'b0;
                fb_v <= 1'b0; fb_wait <= 1'b0;
            end

            // ---------- 写 ----------
            if (f_awvalid && f_awready) begin aw_got <= 1'b1; aw_addr_r <= f_awaddr; end
            if (f_wvalid  && f_wready)  begin w_got  <= 1'b1; w_data_r  <= f_wdata;
                                              w_strb_r <= f_wstrb; end
            if (wr_now) begin
                aw_got <= 1'b0; w_got <= 1'b0;
                f_bvalid <= 1'b1; f_bresp <= RESP_OKAY;
                if (wr_strb[0]) begin
                    case (wr_addr[5:2])
                    A_CTRL: begin
                        if (wr_data[1]) zero_pulse <= 1'b1;
                        if (wr_data[2]) in_ptr  <= 13'd0;
                        if (wr_data[3]) out_rd  <= 13'd0;
                        // START 只在空闲时有效，且要放在最后判 ——
                        // 同一拍写 IN_RST|START 的语义是"清指针再启动"
                        if (wr_data[0] && (state == S_IDLE) && !zeroize_all) begin
                            out_len <= 13'd0;
                            out_rd  <= 13'd0;
                            pre_cnt <= 7'd0;
                            fp      <= 13'd0;
                            fb_v    <= 1'b0;
                            fb_wait <= 1'b1;   // 让输入缓冲的同步读跟上
                            run_done <= 1'b0;
                            // Decaps 没有并行口要预读。少了这个判断，
                            // pre_cnt 会一路数到回绕才碰巧退出（白跑 128 拍）。
                            state   <= (mode == M_DECAPS) ? S_KICK : S_PRE;
                        end
                    end
                    A_MODE: begin mode <= wr_data[1:0]; pset <= wr_data[3:2]; end
                    A_INDATA: if (state == S_IDLE) in_ptr <= in_ptr + 13'd1;
                    default: ;
                    endcase
                end
            end
            if (f_bvalid && f_bready) f_bvalid <= 1'b0;

            // ---------- 读 ----------
            if (f_arvalid && f_arready) begin
                f_rvalid <= 1'b1; f_rresp <= RESP_OKAY;
                case (f_araddr[5:2])
                A_VERSION: f_rdata <= VERSION;
                A_STATUS:  f_rdata <= r_status;
                A_MODE:    f_rdata <= {28'd0, pset, mode};
                A_INPTR:   f_rdata <= {19'd0, in_ptr};
                A_OUTDATA: f_rdata <= {24'd0, outb_dout};
                A_OUTLEN:  f_rdata <= {19'd0, out_len};
                A_OUTRD:   f_rdata <= {19'd0, out_rd};
                A_VIOL:    f_rdata <= {viol_rd_count, viol_wr_count};
                A_PARAM0:  f_rdata <= 32'h2000_2000;    // 两块 8 KB 缓冲
                default:   f_rdata <= 32'd0;
                endcase
                if (rd_outdata) out_rd <= out_rd + 13'd1;
            end
            if (f_rvalid && f_rready) f_rvalid <= 1'b0;

            // ---------- 运行 ----------
            if (!zeroize_all) begin
                case (state)
                S_IDLE: ;

                // 把 d/z（或 m）从缓冲区头部读进并行寄存器。
                //
                // ⚠️ 字节序：**先写进来的字节落在最低位**（右移进来）。
                // 三个核的 d_in / z_in / m_in 都是这个约定（cocotb 里写的是
                // int.from_bytes(d, "little")），而字节**流**那一路
                // （ek/dk/c）是先出的字节在前 —— 两个方向不一样，
                // 装反了的表现是 ek 整个不对，但 Decaps 照样过（它不用并行口）。
                S_PRE: begin
                    if (fb_wait) begin
                        fb_wait <= 1'b0;
                    end else begin
                        if (pre_cnt < 7'd32)
                            seed_a <= {inb_dout, seed_a[255:8]};
                        else
                            seed_b <= {inb_dout, seed_b[255:8]};
                        if (pre_cnt + 7'd1 == pre_len) begin
                            state <= S_KICK;
                        end else begin
                            pre_cnt <= pre_cnt + 7'd1;
                        end
                    end
                end

                S_KICK: begin
                    case (mode)
                    M_KEYGEN: kg_start <= 1'b1;
                    M_ENCAPS: en_start <= 1'b1;
                    default:  de_start <= 1'b1;
                    endcase
                    kickdly <= 2'd3;
                    // Encaps 的 ek 从 32 开始；Decaps 从 0 开始
                    fp      <= (mode == M_ENCAPS) ? 13'd32 : 13'd0;
                    fb_v    <= 1'b0;
                    fb_wait <= 1'b1;
                    state   <= S_RUN;
                end

                S_RUN: begin
                    // 头三拍不看 core_dn：这段时间里 start 才刚拉高、
                    // 核还没来得及把上一次的 done 清掉（见 kickdly 的声明处）
                    if (kickdly != 2'd0) kickdly <= kickdly - 2'd1;

                    // ---- 喂输入流（三拍一个字节，同步读要等一拍）----
                    if (fb_v && feed_fire) begin
                        fb_v <= 1'b0; fp <= fp + 13'd1; fb_wait <= 1'b1;
                    end else if (fb_wait) begin
                        fb_wait <= 1'b0;
                    end else if (!fb_v && ({1'b0, fp} < feed_len)) begin
                        fb_r <= inb_dout; fb_v <= 1'b1;
                    end

                    // ---- 收输出流 ----
                    if (core_ov) out_len <= out_len + 13'd1;

                    if (core_dn && (kickdly == 2'd0)) begin
                        run_done <= 1'b1;
                        state    <= S_FIN;
                    end
                end

                S_FIN: begin out_rd <= 13'd0; state <= S_IDLE; end

                default: state <= S_IDLE;
                endcase
            end
        end
    end

    wire _unused = &{1'b0, f_awprot, f_arprot, wr_strb[3:1], core_ol,
                     kg_ol, en_ol, de_ol, 1'b0};

endmodule

`default_nettype wire
