// axi4lite_firewall —— 挂在 master 与 slave 之间的 AXI4-Lite 防火墙
//
// trng_axi 里已经有一份"内联"的 AxPROT 门控：判一下 NS 位，不合规就回
// DECERR 且不产生副作用。那份是对的，但有两个局限，正是本模块要解决的：
//
//   ① **被拦下的事务仍然进到了从机内部**，只是效果被门控信号掐掉。
//      掐得对不对，取决于从机里每一处副作用都记得判那个门控信号 ——
//      漏判一处就是一个洞，而且是安静的洞。
//   ② 每个从机都要自己抄一遍这段逻辑。
//
// 本模块把边界前移：**不合规的事务根本不会出现在下游接口上**。
// 下游从机看到的每一笔访问都是已经被放行的，从机里不必再判一次。
// 这是"密码边界"这句话在 RTL 上的落点 —— 边界是一根导线的位置，
// 不是一段代码的自觉。
//
// ============================================================================
// 【策略是编译期参数，不是运行时寄存器】
// ============================================================================
// 可写的策略寄存器意味着"改策略"本身成了一个攻击面：谁能写它、写它的那条路
// 又归谁管，是个会无限套娃的问题。本模块的策略全部是 parameter，综合完就
// 固定在硅上，**没有任何一条总线路径能改动它**。
//
// 唯一的运行时输入是 tamper，而它只能把门关得更紧、不能放松（fail-closed），
// 并且一旦锁存只有复位能清 —— 软件清不掉。
//
// ============================================================================
// 【为什么是 store-and-forward，而不是组合直通】
// ============================================================================
// AXI4-Lite 的 AW 与 W 是两条独立通道，判权限要用 AW 上的 AxPROT，落笔却在
// W 上。组合直通的话，W 可能先于 AW 到达，此时还不知道该不该放行 —— 要么
// 提前把 W 透给下游（错），要么把 W 卡住等 AW（那就已经不是直通了）。
// 所以本模块把 AW 与 W 都收下来，凑齐再判、再决定转不转发。
//
// 代价是每笔访问多几拍。这是控制总线，读一个状态寄存器多 3 拍无所谓。
//
// ============================================================================
// 【被拒之后怎么回应：RAZ/WI，不是 DECERR】
// ============================================================================
// 被拒的访问**读回 0、写丢弃，响应一律 OKAY** —— 不产生任何总线错误。
//
// 第一版回的是 DECERR，那是"正确"的 AXI 语义，但在这块板上它是个陷阱：
//   · 读的 DECERR 是同步外部中止 → SIGBUS，程序还接得住；
//   · **写是 posted 的** —— 错误过一会儿才以 **SError** 回来。SError 不属于
//     任何一条指令，aarch64 的内核只能 panic。代价是一次断电。
//
// 而触发它不需要恶意：**内核自己的驱动**就会踩到。实测过一次——设备树里
// 还留着厂家 PL 的 GPIO 节点，换上密码位流之后 xgpio_of_probe 去探测一个
// 不存在的外设，当场 SError panic（调用栈见 boot/persist/build_j1_boot.sh）。
//
// 所以：**任何用户态程序、任何驱动、任何手滑，都不该有能力把板子搞崩。**
// 这是密码机该有的性质，不是"宽容"。RAZ/WI 换来的正是这个。
//
// 安全性一点没减：写到不了从机、读拿不到密钥，与 DECERR 时逐字一样。
// 变的只是**总线不再产生错误**。
//
// 代价要说清楚：**写错地址变安静了** —— 打错一个地址不再报错，而是读回 0。
// 拿"可诊断性"换"崩不了板"。两处补偿：
//   ① 违规计数器（下面那段）留下痕迹，且只有安全世界读得到；
//   ② 所有从机的 VERSION 都是非零的，所以"读到 0"本身就是"被拒了"的显眼信号
//      —— 边界证明也改用这一条（普通世界读到 0 / 安全世界读到 0x00010000）。
//
// ============================================================================
// 【违规记录】
// ============================================================================
// 拦下来只是第一步，拦下来这件事本身必须留痕，否则"没被攻击"和"被攻击了但
// 没人知道"长得一模一样。本模块给出饱和计数与**第一次**违规的现场
// （地址 / 读写 / NS 位）。取第一次而不是最后一次：第一次通常是探测，
// 最后一次往往已经是扫描产生的噪声。
`default_nettype none

module axi4lite_firewall #(
    parameter integer AW            = 8,       // 地址位宽
    // ---- 策略 ----
    parameter integer SECURE_ONLY   = 1,       // 要求 AxPROT[1] == 0（secure）
    parameter integer PRIV_ONLY     = 0,       // 要求 AxPROT[0] == 1（privileged）
    parameter integer ALLOW_WRITE   = 1,
    parameter integer ALLOW_READ    = 1,
    // 地址窗口：(addr & ADDR_MASK) == ADDR_BASE 才放行。
    // 默认 MASK=0 表示不做地址检查（整个 AW 位地址空间都归下游）。
    parameter [31:0]  ADDR_BASE     = 32'h0,
    parameter [31:0]  ADDR_MASK     = 32'h0
) (
    input  wire            clk,
    input  wire            rst_n,

    // 拉高即锁存，之后一切访问都拒绝，只有 rst_n 能清
    input  wire            tamper,

    // ---- 上游（面向 master）----
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

    // ---- 下游（面向 slave）----
    output wire [AW-1:0]   m_awaddr,
    output wire [2:0]      m_awprot,
    output reg             m_awvalid,
    input  wire            m_awready,
    output wire [31:0]     m_wdata,
    output wire [3:0]      m_wstrb,
    output reg             m_wvalid,
    input  wire            m_wready,
    input  wire [1:0]      m_bresp,
    input  wire            m_bvalid,
    output reg             m_bready,

    output wire [AW-1:0]   m_araddr,
    output wire [2:0]      m_arprot,
    output reg             m_arvalid,
    input  wire            m_arready,
    input  wire [31:0]     m_rdata,
    input  wire [1:0]      m_rresp,
    input  wire            m_rvalid,
    output reg             m_rready,

    // ---- 审计 ----
    output reg  [15:0]     viol_wr_count,      // 饱和计数
    output reg  [15:0]     viol_rd_count,
    output reg             viol_first_valid,
    output reg  [AW-1:0]   viol_first_addr,
    output reg  [2:0]      viol_first_prot,
    output reg             viol_first_is_write,
    output reg             tamper_latched
);
    // 只有 OKAY。被拒也回 OKAY —— DECERR 这条路已经没有了（见文件头），
    // 所以连常量都不留：留着会让人以为某个分支还会用到它。
    localparam [1:0] RESP_OKAY = 2'b00;

    // ================= 篡改锁存 =================
    // 只进不出：软件没有任何一条路能把它清掉，只有 rst_n。
    //
    // ⚠️ 锁存要到**下一拍**才生效。所以判据里不能只看 tamper_latched ——
    //    tamper 拉高的那一拍它还是 0，那一拍到达的事务会被照常放行。
    //    这是一拍宽的 TOCTOU 窗口：篡改检测（开盖、电压/温度越界）与总线
    //    事务是两条互不相干的时间线，它们**恰好同拍**并不是小概率事件，
    //    而是攻击者可以主动去凑的 —— 拔盖的那一刻正在扫描寄存器，
    //    就有一笔访问踩在这一拍上。
    //    判据用 (tamper || tamper_latched)，窗口宽度归零。
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)      tamper_latched <= 1'b0;
        else if (tamper) tamper_latched <= 1'b1;
    end

    // ================= 放行判据 =================
    function automatic permit;
        input [AW-1:0] addr;
        input [2:0]    prot;
        input          is_write;
        reg            ok;
        begin
            ok = 1'b1;
            // 组合项在前：tamper 拉高的**同一拍**就关门，不等锁存
            if (tamper || tamper_latched)             ok = 1'b0;
            if (is_write  && (ALLOW_WRITE  == 0))     ok = 1'b0;
            if (!is_write && (ALLOW_READ   == 0))     ok = 1'b0;
            if ((SECURE_ONLY != 0) && prot[1])        ok = 1'b0;   // NS=1 → 拒
            if ((PRIV_ONLY   != 0) && !prot[0])       ok = 1'b0;   // 非特权 → 拒
            if (ADDR_MASK != 32'h0) begin
                if (({{(32-AW){1'b0}}, addr} & ADDR_MASK) != ADDR_BASE) ok = 1'b0;
            end
            permit = ok;
        end
    endfunction

    // ================= 写通道 =================
    localparam [2:0] W_IDLE = 3'd0, W_FWD = 3'd1, W_WAITB = 3'd2,
                     W_ERR  = 3'd3, W_RESP = 3'd4;

    reg [2:0]      wst;
    reg            aw_got, w_got;
    reg [AW-1:0]   aw_addr_r;
    reg [2:0]      aw_prot_r;
    reg [31:0]     w_data_r;
    reg [3:0]      w_strb_r;
    reg            aw_sent, w_sent;

    assign s_awready = (wst == W_IDLE) && !aw_got;
    assign s_wready  = (wst == W_IDLE) && !w_got;

    assign m_awaddr = aw_addr_r;
    assign m_awprot = aw_prot_r;
    assign m_wdata  = w_data_r;
    assign m_wstrb  = w_strb_r;

    // 写方向的判据**不能**在这里预先算一份：W_IDLE 那一拍地址可能还在
    // s_awaddr 上（aw_addr_r 要到下一拍才更新），所以下面是拿
    // "本拍握手就用 s_awaddr、否则用 aw_addr_r" 现场调 permit 的。
    // 原先这里有一根 wr_permit，用的是纯 aw_addr_r，从来没有被引用过。

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            wst <= W_IDLE; aw_got <= 1'b0; w_got <= 1'b0;
            aw_addr_r <= {AW{1'b0}}; aw_prot_r <= 3'd0;
            w_data_r <= 32'd0; w_strb_r <= 4'd0;
            aw_sent <= 1'b0; w_sent <= 1'b0;
            m_awvalid <= 1'b0; m_wvalid <= 1'b0; m_bready <= 1'b0;
            s_bvalid <= 1'b0; s_bresp <= RESP_OKAY;
            viol_wr_count <= 16'd0;
        end else begin
            case (wst)
            W_IDLE: begin
                if (s_awvalid && s_awready) begin
                    aw_got <= 1'b1; aw_addr_r <= s_awaddr; aw_prot_r <= s_awprot;
                end
                if (s_wvalid && s_wready) begin
                    w_got <= 1'b1; w_data_r <= s_wdata; w_strb_r <= s_wstrb;
                end
                // 两条通道都齐了才判 —— 判据在 AW 上，落笔在 W 上
                if ((aw_got || (s_awvalid && s_awready))
                    && (w_got || (s_wvalid && s_wready))) begin
                    aw_got <= 1'b0; w_got <= 1'b0;
                    // 这一拍 aw_addr_r 可能还没更新，所以用组合值再判一次
                    if (permit(s_awvalid && s_awready ? s_awaddr : aw_addr_r,
                               s_awvalid && s_awready ? s_awprot : aw_prot_r,
                               1'b1)) begin
                        aw_addr_r <= s_awvalid && s_awready ? s_awaddr : aw_addr_r;
                        aw_prot_r <= s_awvalid && s_awready ? s_awprot : aw_prot_r;
                        w_data_r  <= s_wvalid  && s_wready  ? s_wdata  : w_data_r;
                        w_strb_r  <= s_wvalid  && s_wready  ? s_wstrb  : w_strb_r;
                        m_awvalid <= 1'b1; m_wvalid <= 1'b1;
                        aw_sent   <= 1'b0; w_sent   <= 1'b0;
                        wst       <= W_FWD;
                    end else begin
                        if (viol_wr_count != 16'hFFFF)
                            viol_wr_count <= viol_wr_count + 16'd1;
                        wst <= W_ERR;
                    end
                end
            end

            W_FWD: begin
                if (m_awvalid && m_awready) begin m_awvalid <= 1'b0; aw_sent <= 1'b1; end
                if (m_wvalid  && m_wready)  begin m_wvalid  <= 1'b0; w_sent  <= 1'b1; end
                if ((aw_sent || (m_awvalid && m_awready))
                    && (w_sent || (m_wvalid && m_wready))) begin
                    m_bready <= 1'b1;
                    wst      <= W_WAITB;
                end
            end

            W_WAITB: if (m_bvalid) begin
                m_bready <= 1'b0;
                s_bresp  <= m_bresp;
                s_bvalid <= 1'b1;
                wst      <= W_RESP;
            end

            // 被拒的写：**丢弃，回 OKAY**。见文件头 —— 回 DECERR 会让 posted 写
            // 以 SError 打回内核，只能 panic。写本身从没到过下游从机。
            W_ERR: begin
                s_bresp  <= RESP_OKAY;
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

    reg [2:0]    rst_st;
    reg [AW-1:0] ar_addr_r;
    reg [2:0]    ar_prot_r;

    assign s_arready = (rst_st == R_IDLE);
    assign m_araddr  = ar_addr_r;
    assign m_arprot  = ar_prot_r;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rst_st <= R_IDLE;
            ar_addr_r <= {AW{1'b0}}; ar_prot_r <= 3'd0;
            m_arvalid <= 1'b0; m_rready <= 1'b0;
            s_rvalid <= 1'b0; s_rresp <= RESP_OKAY; s_rdata <= 32'd0;
            viol_rd_count <= 16'd0;
        end else begin
            case (rst_st)
            R_IDLE: if (s_arvalid && s_arready) begin
                ar_addr_r <= s_araddr;
                ar_prot_r <= s_arprot;
                if (permit(s_araddr, s_arprot, 1'b0)) begin
                    m_arvalid <= 1'b1;
                    rst_st    <= R_FWD;
                end else begin
                    if (viol_rd_count != 16'hFFFF)
                        viol_rd_count <= viol_rd_count + 16'd1;
                    rst_st <= R_ERR;
                end
            end

            R_FWD: if (m_arvalid && m_arready) begin
                m_arvalid <= 1'b0;
                m_rready  <= 1'b1;
                rst_st    <= R_WAITR;
            end

            R_WAITR: if (m_rvalid) begin
                m_rready <= 1'b0;
                s_rdata  <= m_rdata;
                s_rresp  <= m_rresp;
                s_rvalid <= 1'b1;
                rst_st   <= R_RESP;
            end

            // 被拦下的读返回 0 而不是任何下游数据 —— 下游根本没被访问过，
            // 所以这里连"泄露一个陈值"的机会都不存在。
            // 被拒的读：**回 0 + OKAY**。下游从机根本没被访问过，
            // 所以连"泄露一个陈值"的机会都不存在。
            R_ERR: begin
                s_rdata  <= 32'd0;
                s_rresp  <= RESP_OKAY;
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

    // ================= 第一次违规的现场 =================
    // 取第一次而不是最后一次：第一次通常是探测，最后一次往往已经是扫描噪声。
    wire wr_viol_now = (wst == W_IDLE)
                       && (aw_got || (s_awvalid && s_awready))
                       && (w_got  || (s_wvalid  && s_wready))
                       && !permit(s_awvalid && s_awready ? s_awaddr : aw_addr_r,
                                  s_awvalid && s_awready ? s_awprot : aw_prot_r,
                                  1'b1);
    wire rd_viol_now = (rst_st == R_IDLE) && s_arvalid && s_arready
                       && !permit(s_araddr, s_arprot, 1'b0);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            viol_first_valid    <= 1'b0;
            viol_first_addr     <= {AW{1'b0}};
            viol_first_prot     <= 3'd0;
            viol_first_is_write <= 1'b0;
        end else if (!viol_first_valid && (wr_viol_now || rd_viol_now)) begin
            viol_first_valid    <= 1'b1;
            viol_first_is_write <= wr_viol_now;
            viol_first_addr     <= wr_viol_now
                                 ? (s_awvalid && s_awready ? s_awaddr : aw_addr_r)
                                 : s_araddr;
            viol_first_prot     <= wr_viol_now
                                 ? (s_awvalid && s_awready ? s_awprot : aw_prot_r)
                                 : s_arprot;
        end
    end

endmodule

`default_nettype wire
