// sm3_core —— SM3 杂凑（GB/T 32905-2016）
//
//     start → 字节流（in_valid/in_ready/in_data，喂完拉 in_flush）
//           → done + digest[255:0]
//
// 填充在核内部做：软件只管把消息喂进来，不必自己算 0x80、补零和长度域。
// 这一条是有意的 —— 填充是"看起来简单、实际最常写错"的一段，尤其是
// "剩余空间放不下 8 字节长度时要再补一整块"那个分支。放进硬件里写一次、
// 用一条**跨块边界**的用例钉住，比让每个调用方各写一遍强。
//
// ============================================================================
// 【消息扩展的窗口方向：往前看，不是往后看】
// ============================================================================
// 标准写的是 W[j] = P1(W[j−16] ^ W[j−9] ^ (W[j−3] <<< 15)) ^ (W[j−13] <<< 7)
// ^ W[j−6]，照字面做就要保留过去 16 个字。但压缩那边还要 W′[j] = W[j] ^ W[j+4]
// —— 那是**未来**的字，于是扩展必须比压缩快 4 步，两个进度得分别管。
//
// 换个方向就没这个问题：让窗口装 W[j..j+15]（当前和未来 15 个），于是
//   · W[j] = w[0]、W[j+4] = w[4]，W′[j] 当场就有；
//   · 新字 W[j+16] = P1(w[0] ^ w[7] ^ (w[13]<<<15)) ^ (w[3]<<<7) ^ w[10]，
//     把标准式子里的 k = j+16 代进去就是这个形状。
// 一拍一轮，窗口 16 个字（512 bit），扩展与压缩同步推进，不用两套进度。
//
// ============================================================================
// 【常量时间】
// ============================================================================
// SM3 没有密钥，但它在密码机里用来算 HMAC 与 KDF，输入是秘密的。
// 本核的拍数只由**消息长度**决定（长度本身通常是公开的），与消息内容无关：
// 每 64 字节一块、每块 65 拍，没有任何依赖数据取值的分支。test_sm3 里量过。
`default_nettype none

module sm3_core (
    input  wire         clk,
    input  wire         rst_n,

    input  wire         start,        // 脉冲：开始一条新消息
    input  wire         in_valid,
    output wire         in_ready,
    input  wire [7:0]   in_data,
    // 「没有更多数据了」。**不用 in_last** —— 那种「和最后一个字节同拍」的
    // 表达方式**空消息没有那一拍可用**，核会永远停在等字节上（第一版就是这么
    // 挂的）。与 sha3_core 的 in_flush 同一个约定：只在 in_ready 为高时采样。
    input  wire         in_flush,

    output reg          done,         // 电平，保持到下一次 start
    output wire [255:0] digest,

    input  wire         zeroize
);
    localparam [2:0] S_IDLE = 3'd0, S_ABS = 3'd1, S_PAD = 3'd2,
                     S_COMP = 3'd3, S_FIN = 3'd4;

    reg [2:0]  state;
    reg [511:0] blk;                  // 当前 512 位块，先进的字节在高位
    reg [5:0]  pos;                   // 块内字节下标 0..63
    reg [60:0] bytecnt;               // 消息总字节数
    reg        p80, can_len, len_done, back_to_pad;

    reg [31:0] v0, v1, v2, v3, v4, v5, v6, v7;
    assign digest = {v0, v1, v2, v3, v4, v5, v6, v7};

    assign in_ready = (state == S_ABS);

    // ================= 压缩 =================
    reg [31:0] a, b, c, d, e, f, g, h;
    reg [31:0] w [0:15];              // 窗口装 W[j..j+15]
    reg [6:0]  j;

    function automatic [31:0] rotl;
        input [31:0] v;
        input integer n;
        begin
            rotl = (v << n) | (v >> (32 - n));
        end
    endfunction

    function automatic [31:0] p0;
        input [31:0] x;
        begin p0 = x ^ rotl(x, 9) ^ rotl(x, 17); end
    endfunction

    function automatic [31:0] p1;
        input [31:0] x;
        begin p1 = x ^ rotl(x, 15) ^ rotl(x, 23); end
    endfunction

    // ⚠️ j 从 1 数到 64（j==0 那一拍用来装载 A..H 与窗口），所以**标准正文里
    //    的轮号是 j−1**。T_j 的分界、T_j 的循环左移量、FF/GG 的分界三处都得用
    //    jj —— 照着 j 写就整体差一轮，表现是摘要完全不对（第一版就是这么错的）。
    wire [6:0]  jj = j - 7'd1;

    wire [31:0] tj = (jj < 7'd16) ? 32'h79CC4519 : 32'h7A879D8A;
    // T_j <<< (j mod 32)。rotl 的移位量形参是 integer（32 位），所以这里把
    // 5 位的 jj[4:0] 显式补齐再传 —— 其余调用点传的都是字面量常数，本来就是
    // 32 位，只有这一处是从信号里取的。
    wire [31:0] tj_rot = rotl(tj, {27'd0, jj[4:0]});

    wire [31:0] ff = (jj < 7'd16) ? (a ^ b ^ c) : ((a & b) | (a & c) | (b & c));
    wire [31:0] gg = (jj < 7'd16) ? (e ^ f ^ g) : ((e & f) | (~e & g));

    wire [31:0] a12  = rotl(a, 12);
    wire [31:0] ss1  = rotl(a12 + e + tj_rot, 7);
    wire [31:0] ss2  = ss1 ^ a12;
    wire [31:0] wj   = w[0];
    wire [31:0] wj1  = w[0] ^ w[4];          // W′[j]
    wire [31:0] tt1  = ff + d + ss2 + wj1;
    wire [31:0] tt2  = gg + h + ss1 + wj;

    // W[j+16]：把标准式子里的 k = j+16 代进去
    wire [31:0] w_new = p1(w[0] ^ w[7] ^ rotl(w[13], 15))
                        ^ rotl(w[3], 7) ^ w[10];

    // ================= 填充 =================
    // 0x80 之后补零；本块还放得下 8 字节长度就放，放不下就补满、压缩、
    // 到下一块再放。**"放不下"那条分支是填充里最容易写错的地方**，
    // test_sm3 用一条 56 字节的消息专门顶着它。
    wire [63:0] bitlen = {bytecnt, 3'b000};
    wire [7:0]  len_byte = bitlen[63 - 8*(pos - 6'd56) -: 8];

    wire [7:0] pad_byte = (!p80)                       ? 8'h80
                        : (can_len && (pos >= 6'd56))  ? len_byte
                                                       : 8'h00;

    integer i;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= S_IDLE; done <= 1'b0;
            blk <= 512'd0; pos <= 6'd0; bytecnt <= 61'd0;
            p80 <= 1'b0; can_len <= 1'b0; len_done <= 1'b0; back_to_pad <= 1'b0;
            {v0, v1, v2, v3, v4, v5, v6, v7} <= 256'd0;
            a <= 32'd0; b <= 32'd0; c <= 32'd0; d <= 32'd0;
            e <= 32'd0; f <= 32'd0; g <= 32'd0; h <= 32'd0;
            j <= 7'd0;
            for (i = 0; i < 16; i = i + 1) w[i] <= 32'd0;
        end else if (zeroize) begin
            // SM3 本身无密钥，但中间态里有消息内容 —— HMAC / KDF 场景下
            // 那就是秘密。与密钥仓接同一根 zeroize。
            state <= S_IDLE; done <= 1'b0;
            blk <= 512'd0; pos <= 6'd0; bytecnt <= 61'd0;
            p80 <= 1'b0; can_len <= 1'b0; len_done <= 1'b0; back_to_pad <= 1'b0;
            {v0, v1, v2, v3, v4, v5, v6, v7} <= 256'd0;
            a <= 32'd0; b <= 32'd0; c <= 32'd0; d <= 32'd0;
            e <= 32'd0; f <= 32'd0; g <= 32'd0; h <= 32'd0;
            for (i = 0; i < 16; i = i + 1) w[i] <= 32'd0;
        end else begin
            case (state)
            S_IDLE: if (start) begin
                done    <= 1'b0;
                blk     <= 512'd0;
                pos     <= 6'd0;
                bytecnt <= 61'd0;
                p80     <= 1'b0;
                can_len <= 1'b1;      // 空块当然放得下长度
                len_done <= 1'b0;
                back_to_pad <= 1'b0;
                v0 <= 32'h7380166F; v1 <= 32'h4914B2B9;
                v2 <= 32'h172442D7; v3 <= 32'hDA8A0600;
                v4 <= 32'hA96F30BC; v5 <= 32'h163138AA;
                v6 <= 32'hE38DEE4D; v7 <= 32'hB0FB0E4E;
                state <= S_ABS;
            end

            S_ABS: begin
                if (in_flush) begin
                    state <= S_PAD;
                end else if (in_valid) begin
                    blk     <= {blk[503:0], in_data};
                    bytecnt <= bytecnt + 61'd1;
                    if (pos == 6'd63) begin
                        pos <= 6'd0;
                        back_to_pad <= 1'b0;
                        state <= S_COMP;
                    end else begin
                        pos <= pos + 6'd1;
                    end
                end
            end

            S_PAD: begin
                blk <= {blk[503:0], pad_byte};
                if (!p80) begin
                    p80     <= 1'b1;
                    // 0x80 落在 55 之后，本块就放不下 8 字节长度了
                    can_len <= (pos <= 6'd55);
                end
                if (can_len && (pos == 6'd63) && p80) len_done <= 1'b1;
                if (pos == 6'd63) begin
                    pos <= 6'd0;
                    back_to_pad <= 1'b1;
                    // 换到新的一块 —— 空块当然放得下长度域。这一句必须在
                    // 上面那个 can_len <= (pos <= 55) 之后（后写的赢）：
                    // 0x80 恰好落在 63 时，上面刚把 can_len 判成 0，
                    // 而下一块是空的，长度就该放在下一块里。少了这一句的
                    // 表现是长度 56~63 的消息永远算不完 —— 每一块都认为
                    // 自己放不下，一路补零补下去。
                    can_len <= 1'b1;
                    state <= S_COMP;
                end else begin
                    pos <= pos + 6'd1;
                end
            end

            S_COMP: begin
                if (j == 7'd0) begin
                    // 装载：A..H = V，窗口 = 本块的 16 个字
                    a <= v0; b <= v1; c <= v2; d <= v3;
                    e <= v4; f <= v5; g <= v6; h <= v7;
                    for (i = 0; i < 16; i = i + 1)
                        w[i] <= blk[511 - 32*i -: 32];
                    j <= 7'd1;
                end else begin
                    d <= c;  c <= rotl(b, 9);  b <= a;  a <= tt1;
                    h <= g;  g <= rotl(f, 19); f <= e;  e <= p0(tt2);
                    for (i = 0; i < 15; i = i + 1) w[i] <= w[i+1];
                    w[15] <= w_new;
                    if (j == 7'd64) begin
                        // 第 64 轮做完，异或回 V
                        v0 <= v0 ^ tt1;          v1 <= v1 ^ a;
                        v2 <= v2 ^ rotl(b, 9);   v3 <= v3 ^ c;
                        v4 <= v4 ^ p0(tt2);      v5 <= v5 ^ e;
                        v6 <= v6 ^ rotl(f, 19);  v7 <= v7 ^ g;
                        j     <= 7'd0;
                        blk   <= 512'd0;
                        state <= back_to_pad
                                 ? (len_done ? S_FIN : S_PAD)
                                 : S_ABS;
                    end else begin
                        j <= j + 7'd1;
                    end
                end
            end

            S_FIN: begin done <= 1'b1; state <= S_IDLE; end

            default: state <= S_IDLE;
            endcase
        end
    end

endmodule

`default_nettype wire
