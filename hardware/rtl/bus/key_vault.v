// key_vault —— PL 内的密钥仓（只写入口 + 只给 PL 内部的使用口）
//
// 这是"密钥不出密码边界"那句话在 RTL 上的形状。核心不变量只有一条：
//
//     **密钥材料没有任何一条通往总线的路径。**
//
// 不是"读回来会被门控掉"，而是从密钥寄存器到 `s_axi_rdata` 之间**根本没有
// 导线**。软件能做的只有：写进去、标记锁定、擦掉、问一句"这个槽有没有装"。
// 要用这把密钥，只能由 PL 里的算法核通过 use 口取 —— 那根线不出芯片。
//
// 门控与"没有路径"的差别，是这一层唯一真正值钱的地方：门控要求每一处
// 副作用都记得判那个门控信号，漏判一处就是一个安静的洞；没有路径则没有
// 可漏判的东西。key_vault_axi 的读多路选择器里只出现元数据，
// 这一点在 test_key_vault 里是**逐地址扫描**验证的，不是靠代码审查。
//
// ============================================================================
// 【为什么用寄存器而不是 BRAM】
// ============================================================================
// S3 的教训是"大存储别摊成寄存器"（16 KiB 摊开是 30000 个 LUT 的选择树）。
// 这里不一样：8 槽 × 256 bit = 2048 bit，摊成寄存器是 2048 个 FF（片子的
// 1.5%），使用口是 256 位宽的 8 选 1。选寄存器有两条实打实的理由：
//
//   · **擦除是一拍完成的**。BRAM 要逐地址写零，擦除过程中存在一个
//     "擦了一半"的窗口；掉电/复位卡在那个窗口里，残留就还在片上。
//     寄存器阵列一个时钟沿全清，没有这个窗口。
//   · 使用口要的是整把密钥，BRAM 得分 8 拍读出来再拼 —— 那 8 拍里密钥
//     以明文躺在一组临时寄存器上，反而多了一处驻留。
//
// 面积代价在综合报告里如实给出，不做估算。
//
// ============================================================================
// 【tamper 只进不出】
// ============================================================================
// tamper 拉高即锁存：立刻擦除全部槽位，之后**拒绝一切装载**，use_valid 恒 0。
// 软件没有任何一条路能把它清掉，只有 rst_n（也就是重新上电/复位）。
// 真密码机在检测到开盖、电压/温度越界之后本来就要重新灌装，这里照做。
`default_nettype none

module key_vault #(
    parameter integer SLOTS    = 8,        // 槽位数，必须是 2 的幂
    parameter integer SLOT_BITS = 3,       // = log2(SLOTS)
    parameter integer WORDS    = 8         // 每槽多少个 32 位字（8 → 256 bit）
) (
    input  wire                   clk,
    input  wire                   rst_n,

    // ---- 装载口（只写）----
    input  wire [SLOT_BITS-1:0]   ld_slot,
    input  wire                   ld_begin,    // 脉冲：清该槽的字下标与 valid
    input  wire                   ld_we,       // 脉冲：写入一个字
    input  wire [31:0]            ld_wdata,
    input  wire                   ld_commit,   // 脉冲：字写满才置 valid
    input  wire                   ld_lock,     // 脉冲：锁定该槽
    input  wire                   ld_erase,    // 脉冲：擦除该槽

    // ---- 全局擦除 ----
    input  wire                   zeroize,     // 软件发起
    input  wire                   tamper,      // 硬件发起，锁存

    // ---- 使用口：只接 PL 内部的算法核，不出芯片 ----
    input  wire [SLOT_BITS-1:0]   use_sel,
    output wire [WORDS*32-1:0]    use_key,
    output wire                   use_valid,

    // ---- 状态（**只有元数据，没有密钥材料**）----
    output reg  [SLOTS-1:0]       valid_map,
    output reg  [SLOTS-1:0]       lock_map,
    output wire [3:0]             sel_fill,    // 当前 ld_slot 已写入的字数
    output reg                    tamper_latched,
    output reg  [7:0]             zeroize_count,   // 擦除发生过几次（饱和）
    output reg                    deny            // 上一拍有一个操作被拒（脉冲）
);
    // 密钥寄存器阵列。**这个信号只出现在 use_key 的选择器里**，
    // 不出现在任何面向总线的表达式中 —— 这是本模块的全部意义。
    reg [31:0] keys [0:SLOTS*WORDS-1];
    reg [3:0]  fill [0:SLOTS-1];

    integer i;

    wire locked_sel = lock_map[ld_slot];

    assign sel_fill = fill[ld_slot];

    // 使用口：整把密钥的 SLOTS 选 1。tamper 之后恒零 —— 不是"读出来无效"，
    // 是槽位真的已经被清了，这里再加一道是为了 tamper 与 rst_n 之间的那几拍。
    // 字序：**先写进来的字在最高位**（use_key[255:224] 是第 0 个字）。
    // 与仓库里其它地方一致（block_in[127:120] 是第 0 个字节），也正是
    // aes_core / sm4_core 期待的顺序 —— 反过来的话密文全错而不会报任何错。
    reg [WORDS*32-1:0] use_mux;
    always @(*) begin
        use_mux = {(WORDS*32){1'b0}};
        for (i = 0; i < WORDS; i = i + 1) begin
            use_mux[(WORDS-1-i)*32 +: 32] = keys[use_sel*WORDS + i];
        end
    end
    assign use_key   = tamper_latched ? {(WORDS*32){1'b0}} : use_mux;
    assign use_valid = valid_map[use_sel] && !tamper_latched;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (i = 0; i < SLOTS*WORDS; i = i + 1) keys[i] <= 32'd0;
            for (i = 0; i < SLOTS; i = i + 1)       fill[i] <= 4'd0;
            valid_map      <= {SLOTS{1'b0}};
            lock_map       <= {SLOTS{1'b0}};
            tamper_latched <= 1'b0;
            zeroize_count  <= 8'd0;
            deny           <= 1'b0;
        end else begin
            deny <= 1'b0;

            if (tamper) tamper_latched <= 1'b1;

            if (zeroize || tamper) begin
                // 一拍全清 —— 没有"擦了一半"的窗口。lock 也一起清：
                // 擦除之后这块仓就是全新的，不该留下上一任的锁。
                for (i = 0; i < SLOTS*WORDS; i = i + 1) keys[i] <= 32'd0;
                for (i = 0; i < SLOTS; i = i + 1)       fill[i] <= 4'd0;
                valid_map <= {SLOTS{1'b0}};
                lock_map  <= {SLOTS{1'b0}};
                if (zeroize_count != 8'hFF) zeroize_count <= zeroize_count + 8'd1;
            end else if (tamper_latched) begin
                // 锁存之后一律拒绝，连擦除都不必了（已经是空的）
                if (ld_begin || ld_we || ld_commit || ld_lock || ld_erase)
                    deny <= 1'b1;
            end else begin
                if (ld_begin) begin
                    if (locked_sel) deny <= 1'b1;
                    else begin
                        fill[ld_slot]      <= 4'd0;
                        valid_map[ld_slot] <= 1'b0;
                    end
                end else if (ld_we) begin
                    // 锁定的槽写不进；写满了还写也写不进（要先 begin）
                    if (locked_sel || (fill[ld_slot] >= WORDS[3:0])) deny <= 1'b1;
                    else begin
                        // ld_slot*WORDS 是 32 位整型运算，fill 是 4 位 ——
                        // 显式补齐再相加，两侧同宽（use_mux 那边的下标本来就
                        // 是两个整型相加，所以只有这一处要补）。
                        keys[ld_slot*WORDS + {28'd0, fill[ld_slot]}] <= ld_wdata;
                        fill[ld_slot] <= fill[ld_slot] + 4'd1;
                    end
                end else if (ld_commit) begin
                    // 只有整把密钥都写齐了才算数。写了一半就 commit 是个
                    // 明确的错误：半把密钥比没有密钥更危险。
                    if (locked_sel || (fill[ld_slot] != WORDS[3:0])) deny <= 1'b1;
                    else valid_map[ld_slot] <= 1'b1;
                end else if (ld_lock) begin
                    if (!valid_map[ld_slot]) deny <= 1'b1;   // 空槽没什么可锁
                    else lock_map[ld_slot] <= 1'b1;
                end else if (ld_erase) begin
                    if (locked_sel) deny <= 1'b1;
                    else begin
                        for (i = 0; i < WORDS; i = i + 1)
                            keys[ld_slot*WORDS + i] <= 32'd0;
                        fill[ld_slot]      <= 4'd0;
                        valid_map[ld_slot] <= 1'b0;
                    end
                end
            end
        end
    end

endmodule

`default_nettype wire
