// aes_core —— AES-128 / AES-256 分组加解密（FIPS 197）
//
// 接口是**两条命令**，不是一条：
//
//     key_start  → 装载并展开密钥（AES-128 约 44 拍、AES-256 约 60 拍）
//     blk_start  → 处理一个分组（Nr+1 拍，128 位密钥 11 拍、256 位 15 拍）
//
// 分成两条是因为真密码机的用法是"装一次密钥、过很多个分组"。合成一条的话
// 每个分组都要重跑一遍密钥扩展，吞吐掉四分之三。
//
// 更要紧的是：**合成一条就得引入"密钥没变就跳过扩展"的缓存**，而那个判断
// 的耗时依赖于"这次的密钥是不是上次那把"。虽然这条信息通常调用方本来就知道，
// 但它是一条本可以不存在的信道。拆成两条命令，这个问题根本不出现。
//
// ============================================================================
// 【关于常量时间，把话说准】
// ============================================================================
// 软件 AES 的经典问题是 S 盒查表走 cache，访问时间依赖于查的是哪一格，
// 于是密钥可以被 cache 时序反推出来。**硬件里没有这个问题** —— 查表就是一片
// 组合逻辑，任何输入都走同一条路径、同一个延迟。
//
// 但这是"这类攻击面在硬件上不存在"，**不是"我们做了防护"**。两者要分清：
//   · 本核的拍数只由 key_256 与 Nr 决定，与密钥、明文的取值完全无关；
//   · 本核**没有**做功耗/电磁侧信道防护（掩码、随机化）。DPA 是另一回事，
//     真要防得在数据通路上加掩码，那是一次独立的改造。这里不做，也不假装做了。
//
// ============================================================================
// 【字节序】
// ============================================================================
// block_in[127:120] 是标准里的第 0 个输入字节，block_in[7:0] 是第 15 个。
// 也就是"十六进制串从左往右"的自然顺序。状态内部同样这么存：
// 第 i 个字节在 state[127-8i -: 8]，按 FIPS 197 的列主序，
// 即 s[r][c] = 第 (4c+r) 个字节。
`default_nettype none

module aes_core (
    input  wire         clk,
    input  wire         rst_n,

    // ---- 命令一：装载并展开密钥 ----
    input  wire         key_start,     // 脉冲
    input  wire [255:0] key_in,        // AES-128 只用高 128 位
    input  wire         key_256,       // 0 = AES-128, 1 = AES-256
    output reg          key_ready,     // 电平，保持到下一次 key_start

    // ---- 命令二：处理一个分组 ----
    input  wire         blk_start,     // 脉冲
    input  wire         decrypt,
    input  wire [127:0] block_in,
    output reg  [127:0] block_out,
    output reg          blk_done,      // 电平，保持到下一次 blk_start

    // ---- 擦除：与密钥仓的 zeroize / tamper 接同一根线 ----
    input  wire         zeroize
);
    // ================= 状态机 =================
    localparam [2:0] S_IDLE = 3'd0, S_KEXP = 3'd1, S_KDONE = 3'd2,
                     S_RND  = 3'd3, S_BDONE = 3'd4;

    reg [2:0] state;

    reg        k256_r, dec_r;
    wire [3:0] nr = k256_r ? 4'd14 : 4'd10;      // 轮数
    wire [3:0] nk = k256_r ? 4'd8  : 4'd4;       // 密钥字数

    // ================= 轮密钥 =================
    // 15 × 128 bit。展开时按字写入，跑轮时整块读出（15 选 1 的 128 位选择器）。
    reg [127:0] rkey [0:14];
    reg [95:0]  rk_sr;                  // 攒够 4 个字再落一块
    reg [6:0]   wi;                     // 正在生成第几个字（0..59）
    // 一共 4·(Nr+1) 个字：AES-128 是 44、AES-256 是 60。位宽写足 7 位 ——
    // 60 装不进 6 位以内的和式，这类地方在 encaps 的 tbytes 上栽过一次。
    wire [6:0]  nwords = {1'b0, nr, 2'b00} + 7'd4;
    reg [31:0]  wbuf [0:7];             // 最近 8 个字，wbuf[7] 是 w[i-1]

    // ================= S 盒 =================
    // 16 个正向 + 16 个逆向。正向那 16 个里的前 4 个在密钥扩展阶段被借去做
    // SubWord —— 两个阶段在时间上不重叠，所以是白拿的。
    reg  [7:0] sb_in  [0:15];
    wire [7:0] sb_out [0:15];
    wire [7:0] isb_out[0:15];

    genvar gi;
    generate
        for (gi = 0; gi < 16; gi = gi + 1) begin : g_sbox
            aes_sbox     u_s  (.x(sb_in[gi]), .y(sb_out[gi]));
            aes_inv_sbox u_is (.x(sb_in[gi]), .y(isb_out[gi]));
        end
    endgenerate

    // ================= 数据通路 =================
    reg [127:0] st;                     // 当前状态
    reg [3:0]   rnd;                    // 当前轮号

    // ---- 密钥扩展：temp = w[i-1]，必要时 RotWord + SubWord + Rcon ----
    wire [31:0] w_prev = wbuf[7];
    wire [31:0] w_back = k256_r ? wbuf[0] : wbuf[4];    // w[i-Nk]

    // wi % nk == 0 —— nk 是 4 或 8，都是 2 的幂，所以取低位即可
    wire kw_zero  = k256_r ? (wi[2:0] == 3'd0) : (wi[1:0] == 2'd0);
    wire kw_four  = k256_r && (wi[2:0] == 3'd4);

    wire [31:0] rot_w = {w_prev[23:0], w_prev[31:24]};
    wire [31:0] sw_in = kw_zero ? rot_w : w_prev;

    // Rcon：i/nk - 1，最多到 13
    reg [7:0] rcon;
    always @(*) begin
        case (k256_r ? {1'b0, wi[5:3]} : wi[5:2])
        4'd1: rcon = 8'h01; 4'd2: rcon = 8'h02; 4'd3:  rcon = 8'h04;
        4'd4: rcon = 8'h08; 4'd5: rcon = 8'h10; 4'd6:  rcon = 8'h20;
        4'd7: rcon = 8'h40; 4'd8: rcon = 8'h80; 4'd9:  rcon = 8'h1b;
        4'd10: rcon = 8'h36; 4'd11: rcon = 8'h6c; 4'd12: rcon = 8'hd8;
        4'd13: rcon = 8'hab; 4'd14: rcon = 8'h4d;
        default: rcon = 8'h00;
        endcase
    end

    wire [31:0] subbed = {sb_out[0], sb_out[1], sb_out[2], sb_out[3]};
    wire [31:0] temp_w = kw_zero ? (subbed ^ {rcon, 24'd0})
                       : kw_four ? subbed
                                 : w_prev;
    wire [31:0] w_new  = w_back ^ temp_w;

    // ---- 轮变换 ----
    // 第 i 个字节在 st[127-8i -: 8]。ShiftRows：输出第 (4c+r) 个字节
    // 取自输入第 (4·((c+r) mod 4) + r) 个字节。
    function automatic [7:0] stb;
        input [127:0] s;
        input integer i;
        begin
            stb = s[127 - 8*i -: 8];
        end
    endfunction

    // SubBytes 之后的 16 个字节（正向与逆向各一份）
    wire [127:0] sub_st = {sb_out[0],  sb_out[1],  sb_out[2],  sb_out[3],
                           sb_out[4],  sb_out[5],  sb_out[6],  sb_out[7],
                           sb_out[8],  sb_out[9],  sb_out[10], sb_out[11],
                           sb_out[12], sb_out[13], sb_out[14], sb_out[15]};
    wire [127:0] isub_st = {isb_out[0],  isb_out[1],  isb_out[2],  isb_out[3],
                            isb_out[4],  isb_out[5],  isb_out[6],  isb_out[7],
                            isb_out[8],  isb_out[9],  isb_out[10], isb_out[11],
                            isb_out[12], isb_out[13], isb_out[14], isb_out[15]};

    function automatic [127:0] shift_rows;
        input [127:0] s;
        integer r, c;
        reg [127:0] o;
        begin
            o = 128'd0;
            for (c = 0; c < 4; c = c + 1)
                for (r = 0; r < 4; r = r + 1)
                    o[127 - 8*(4*c+r) -: 8] = stb(s, 4*((c+r)%4) + r);
            shift_rows = o;
        end
    endfunction

    function automatic [127:0] inv_shift_rows;
        input [127:0] s;
        integer r, c;
        reg [127:0] o;
        begin
            o = 128'd0;
            for (c = 0; c < 4; c = c + 1)
                for (r = 0; r < 4; r = r + 1)
                    o[127 - 8*(4*c+r) -: 8] = stb(s, 4*((c+4-r)%4) + r);
            inv_shift_rows = o;
        end
    endfunction

    // GF(2^8) 乘 2 / 乘 3 —— 只用得到这两个常数，写成两条线就够
    function automatic [7:0] xt;       // ×2
        input [7:0] b;
        begin
            xt = {b[6:0], 1'b0} ^ (b[7] ? 8'h1b : 8'h00);
        end
    endfunction

    function automatic [7:0] gm;       // 乘 9 / 11 / 13 / 14 用的通用乘法
        input [7:0] a;
        input [7:0] b;
        reg [7:0] r, x;
        integer k;
        begin
            r = 8'd0; x = a;
            for (k = 0; k < 8; k = k + 1) begin
                if (b[k]) r = r ^ x;
                x = xt(x);
            end
            gm = r;
        end
    endfunction

    function automatic [127:0] mix_cols;
        input [127:0] s;
        integer c;
        reg [7:0] a0, a1, a2, a3;
        reg [127:0] o;
        begin
            o = 128'd0;
            for (c = 0; c < 4; c = c + 1) begin
                a0 = stb(s, 4*c+0); a1 = stb(s, 4*c+1);
                a2 = stb(s, 4*c+2); a3 = stb(s, 4*c+3);
                o[127 - 8*(4*c+0) -: 8] = xt(a0) ^ (xt(a1) ^ a1) ^ a2 ^ a3;
                o[127 - 8*(4*c+1) -: 8] = a0 ^ xt(a1) ^ (xt(a2) ^ a2) ^ a3;
                o[127 - 8*(4*c+2) -: 8] = a0 ^ a1 ^ xt(a2) ^ (xt(a3) ^ a3);
                o[127 - 8*(4*c+3) -: 8] = (xt(a0) ^ a0) ^ a1 ^ a2 ^ xt(a3);
            end
            mix_cols = o;
        end
    endfunction

    function automatic [127:0] inv_mix_cols;
        input [127:0] s;
        integer c;
        reg [7:0] a0, a1, a2, a3;
        reg [127:0] o;
        begin
            o = 128'd0;
            for (c = 0; c < 4; c = c + 1) begin
                a0 = stb(s, 4*c+0); a1 = stb(s, 4*c+1);
                a2 = stb(s, 4*c+2); a3 = stb(s, 4*c+3);
                o[127 - 8*(4*c+0) -: 8] = gm(a0,8'd14)^gm(a1,8'd11)
                                        ^ gm(a2,8'd13)^gm(a3,8'd9);
                o[127 - 8*(4*c+1) -: 8] = gm(a0,8'd9) ^gm(a1,8'd14)
                                        ^ gm(a2,8'd11)^gm(a3,8'd13);
                o[127 - 8*(4*c+2) -: 8] = gm(a0,8'd13)^gm(a1,8'd9)
                                        ^ gm(a2,8'd14)^gm(a3,8'd11);
                o[127 - 8*(4*c+3) -: 8] = gm(a0,8'd11)^gm(a1,8'd13)
                                        ^ gm(a2,8'd9) ^gm(a3,8'd14);
            end
            inv_mix_cols = o;
        end
    endfunction

    // ---- 本轮用的轮密钥 ----
    wire [3:0]   rk_idx = dec_r ? (nr - rnd) : rnd;
    wire [127:0] rk_now = rkey[rk_idx];

    // ---- S 盒输入的归属：密钥扩展阶段前 4 个借给 SubWord ----
    integer bi;
    always @(*) begin
        for (bi = 0; bi < 16; bi = bi + 1) sb_in[bi] = stb(st, bi);
        if (state == S_KEXP) begin
            sb_in[0] = sw_in[31:24];
            sb_in[1] = sw_in[23:16];
            sb_in[2] = sw_in[15:8];
            sb_in[3] = sw_in[7:0];
        end else if (dec_r) begin
            // 逆向：先 InvShiftRows 再 InvSubBytes，所以送进 S 盒的是移位后的
            for (bi = 0; bi < 16; bi = bi + 1)
                sb_in[bi] = stb(inv_shift_rows(st), bi);
        end
    end

    // 正向一轮：SubBytes → ShiftRows → (MixColumns) → AddRoundKey
    wire [127:0] enc_mid  = shift_rows(sub_st);
    wire [127:0] enc_next = (rnd == nr) ? (enc_mid ^ rk_now)
                                        : (mix_cols(enc_mid) ^ rk_now);

    // 逆向一轮：InvShiftRows → InvSubBytes → AddRoundKey → (InvMixColumns)
    wire [127:0] dec_mid  = isub_st ^ rk_now;
    wire [127:0] dec_next = (rnd == nr) ? dec_mid : inv_mix_cols(dec_mid);

    integer wj;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= S_IDLE; key_ready <= 1'b0; blk_done <= 1'b0;
            k256_r <= 1'b0; dec_r <= 1'b0;
            st <= 128'd0; block_out <= 128'd0;
            rnd <= 4'd0; wi <= 7'd0; rk_sr <= 96'd0;
            for (wj = 0; wj < 8; wj = wj + 1)  wbuf[wj] <= 32'd0;
            for (wj = 0; wj < 15; wj = wj + 1) rkey[wj] <= 128'd0;
        end else if (zeroize) begin
            // 密钥材料一拍全清：轮密钥、状态、输出、以及那把还没扩展完的原始密钥
            state <= S_IDLE; key_ready <= 1'b0; blk_done <= 1'b0;
            st <= 128'd0; block_out <= 128'd0;
            rk_sr <= 96'd0;
            for (wj = 0; wj < 8; wj = wj + 1)  wbuf[wj] <= 32'd0;
            for (wj = 0; wj < 15; wj = wj + 1) rkey[wj] <= 128'd0;
        end else begin
            case (state)
            S_IDLE: begin
                if (key_start) begin
                    key_ready <= 1'b0;
                    k256_r    <= key_256;
                    // 前 Nk 个字直接就是密钥本身
                    for (wj = 0; wj < 8; wj = wj + 1)
                        wbuf[wj] <= key_256 ? key_in[255 - 32*wj -: 32]
                                            : ((wj >= 4)
                                               ? key_in[255 - 32*(wj-4) -: 32]
                                               : 32'd0);
                    rkey[0] <= key_in[255:128];
                    if (key_256) rkey[1] <= key_in[127:0];
                    rk_sr <= 96'd0;
                    wi    <= key_256 ? 7'd8 : 7'd4;
                    state <= S_KEXP;
                end else if (blk_start && key_ready) begin
                    blk_done <= 1'b0;
                    dec_r    <= decrypt;
                    // 第 0 轮就是 AddRoundKey：正向用 rkey[0]，逆向用 rkey[Nr]
                    st       <= block_in ^ (decrypt ? rkey[nr] : rkey[0]);
                    rnd      <= 4'd1;
                    state    <= S_RND;
                end
            end

            // 一拍一个字。攒够 4 个字落一块轮密钥。
            S_KEXP: begin
                for (wj = 0; wj < 7; wj = wj + 1) wbuf[wj] <= wbuf[wj+1];
                wbuf[7] <= w_new;
                rk_sr   <= {rk_sr[63:0], w_new};
                if (wi[1:0] == 2'd3) rkey[wi[5:2]] <= {rk_sr[95:0], w_new};
                if (wi + 7'd1 == nwords) state <= S_KDONE;
                wi <= wi + 7'd1;
            end

            S_KDONE: begin key_ready <= 1'b1; state <= S_IDLE; end

            S_RND: begin
                st <= dec_r ? dec_next : enc_next;
                if (rnd == nr) state <= S_BDONE;
                else           rnd   <= rnd + 4'd1;
            end

            S_BDONE: begin
                block_out <= st;
                blk_done  <= 1'b1;
                state     <= S_IDLE;
            end

            default: state <= S_IDLE;
            endcase
        end
    end

endmodule

`default_nettype wire
