// sm4_core —— SM4 分组加解密（GB/T 32907-2016）
//
// 128 位密钥、128 位分组、32 轮。接口与 aes_core 一样是**两条命令**：
//
//     key_start  → 展开 32 个轮密钥（32 拍）
//     blk_start  → 处理一个分组（32 拍）
//
// 理由也一样：真密码机是"装一次密钥、过很多个分组"，而且拆开之后不需要
// "密钥没变就跳过扩展"那种缓存，也就没有"这次密钥是不是上次那把"的信道。
//
// ============================================================================
// 【SM4 的解密就是加密，只把轮密钥倒过来用】
// ============================================================================
// 这是 Feistel 结构自带的性质，不是巧合。所以本核**没有**逆向数据通路 ——
// `decrypt` 只影响读轮密钥的下标（`rk[31-i]` 而不是 `rk[i]`）。
//
// 这一点值得写下来：AES 的逆变换是另一套逻辑（InvSubBytes / InvMixColumns），
// 面积差不多要翻倍；SM4 一分钱不用多花。两个核并排放在这里，正好把这个
// 结构性差别摆出来。
//
// ============================================================================
// 【CK 常量用算的，不用查表】
// ============================================================================
// GB/T 32907 §7.3.2 定义 ck_{4i+j} = (4i+j)·7 mod 256，所以第 i 轮的四个字节是
// 28i、28i+7、28i+14、28i+21（都 mod 256）。28i = 16i + 8i + 4i，三项移位和。
//
// 写成算式而不是 32 项的表，是因为表要抄 32 行、算式只有一行 —— 而正确性
// 由 GB/T 附录 A 的密文端到端保证：CK 错一格，密文立刻不对。
//
// ============================================================================
// 【常量时间】
// ============================================================================
// 与 aes_core 同一句话：硬件查表没有 cache，拍数恒为 32，与密钥和明文取值
// 完全无关（test_sm4 里量过）。**没有**做功耗/电磁侧信道防护，不假装做了。
`default_nettype none

module sm4_core (
    input  wire         clk,
    input  wire         rst_n,

    // ---- 命令一：装载并展开密钥 ----
    input  wire         key_start,     // 脉冲
    input  wire [127:0] key_in,
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
    localparam [31:0] FK0 = 32'hA3B1BAC6, FK1 = 32'h56AA3350,
                      FK2 = 32'h677D9197, FK3 = 32'hB27022DC;

    localparam [1:0] S_IDLE = 2'd0, S_KEXP = 2'd1, S_RND = 2'd2, S_DONE = 2'd3;

    reg [1:0]  state;
    reg [5:0]  cnt;                    // 轮号 0..31
    reg        dec_r;

    reg [31:0] rk [0:31];              // 32 个轮密钥
    reg [31:0] x0, x1, x2, x3;         // 数据通路 / 密钥扩展共用这四个寄存器

    // ================= τ：四个 S 盒 =================
    // 密钥扩展与轮变换在时间上不重叠，所以这四个实例是两边共用的。
    wire [31:0] tau_in;
    wire [7:0]  s0, s1, s2, s3;
    sm4_sbox u_s0 (.x(tau_in[31:24]), .y(s0));
    sm4_sbox u_s1 (.x(tau_in[23:16]), .y(s1));
    sm4_sbox u_s2 (.x(tau_in[15:8]),  .y(s2));
    sm4_sbox u_s3 (.x(tau_in[7:0]),   .y(s3));
    wire [31:0] tau = {s0, s1, s2, s3};

    function automatic [31:0] rotl;
        input [31:0] v;
        input integer n;
        begin
            rotl = (v << n) | (v >> (32 - n));
        end
    endfunction

    // 轮函数的 L 与密钥扩展的 L′ —— 同一个 τ，两个不同的线性变换
    wire [31:0] l_out  = tau ^ rotl(tau, 2) ^ rotl(tau, 10)
                             ^ rotl(tau, 18) ^ rotl(tau, 24);
    wire [31:0] lp_out = tau ^ rotl(tau, 13) ^ rotl(tau, 23);

    // ================= CK[i]：28i、28i+7、28i+14、28i+21 =================
    wire [8:0] ck_b0 = {cnt[4:0], 4'd0} + {1'b0, cnt[4:0], 3'd0}
                                        + {2'b0, cnt[4:0], 2'd0};
    wire [31:0] ck = {ck_b0[7:0],
                      ck_b0[7:0] + 8'd7,
                      ck_b0[7:0] + 8'd14,
                      ck_b0[7:0] + 8'd21};

    // ================= τ 的输入归属 =================
    // 密钥扩展：K[i+1]^K[i+2]^K[i+3]^CK[i]；轮变换：X[i+1]^X[i+2]^X[i+3]^rk
    wire [31:0] rk_now = dec_r ? rk[6'd31 - cnt] : rk[cnt[4:0]];
    assign tau_in = (state == S_KEXP) ? (x1 ^ x2 ^ x3 ^ ck)
                                      : (x1 ^ x2 ^ x3 ^ rk_now);

    wire [31:0] k_new = x0 ^ lp_out;   // 密钥扩展的新字
    wire [31:0] x_new = x0 ^ l_out;    // 轮变换的新字

    integer i;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= S_IDLE; key_ready <= 1'b0; blk_done <= 1'b0;
            cnt <= 6'd0; dec_r <= 1'b0;
            x0 <= 32'd0; x1 <= 32'd0; x2 <= 32'd0; x3 <= 32'd0;
            block_out <= 128'd0;
            for (i = 0; i < 32; i = i + 1) rk[i] <= 32'd0;
        end else if (zeroize) begin
            state <= S_IDLE; key_ready <= 1'b0; blk_done <= 1'b0;
            x0 <= 32'd0; x1 <= 32'd0; x2 <= 32'd0; x3 <= 32'd0;
            block_out <= 128'd0;
            for (i = 0; i < 32; i = i + 1) rk[i] <= 32'd0;
        end else begin
            case (state)
            S_IDLE: begin
                if (key_start) begin
                    key_ready <= 1'b0;
                    x0 <= key_in[127:96] ^ FK0;
                    x1 <= key_in[95:64]  ^ FK1;
                    x2 <= key_in[63:32]  ^ FK2;
                    x3 <= key_in[31:0]   ^ FK3;
                    cnt   <= 6'd0;
                    state <= S_KEXP;
                end else if (blk_start && key_ready) begin
                    blk_done <= 1'b0;
                    dec_r <= decrypt;
                    x0 <= block_in[127:96];
                    x1 <= block_in[95:64];
                    x2 <= block_in[63:32];
                    x3 <= block_in[31:0];
                    cnt   <= 6'd0;
                    state <= S_RND;
                end
            end

            S_KEXP: begin
                rk[cnt[4:0]] <= k_new;
                x0 <= x1; x1 <= x2; x2 <= x3; x3 <= k_new;
                if (cnt == 6'd31) begin
                    key_ready <= 1'b1;
                    state     <= S_IDLE;
                end
                cnt <= cnt + 6'd1;
            end

            S_RND: begin
                x0 <= x1; x1 <= x2; x2 <= x3; x3 <= x_new;
                if (cnt == 6'd31) state <= S_DONE;
                cnt <= cnt + 6'd1;
            end

            // 反序变换 R：输出是 (X35, X34, X33, X32)，也就是把最后四个字倒过来
            S_DONE: begin
                block_out <= {x3, x2, x1, x0};
                blk_done  <= 1'b1;
                state     <= S_IDLE;
            end

            default: state <= S_IDLE;
            endcase
        end
    end

endmodule

`default_nettype wire
